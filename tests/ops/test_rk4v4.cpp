#include "ops/op_tester.h"
#include "ops/rk4v4_reference.h"
#include "ninfer/ops/kv_cache_append.h"
#include "ninfer/ops/softmax_attention.h"
#include "core/device.h"
#include "core/decode_graph.h"
#include <numeric>

using namespace ninfer;
using namespace ninfer::test;
namespace {
// Fixed before device measurement; same budget as the native INT8 FP64 oracle.
constexpr ReductionCriterion criterion{2.0e-2, 1.1e-3, 4.0e-2};

int run(int heads, int width, int depth, int batch, bool masked, bool graph) {
    const int qheads = heads == 4 ? 24 : 16, live = depth + width;
    const int pages = (live + 63)/64, physical = batch*pages + 1;
    const auto device = cuda_environment().context.device();
    const auto execution = cuda_environment().default_execution;
    std::vector<int> tables(pages*batch), rows(batch), valid(batch, width), positions(width*batch);
    for (int b = 0; b < batch; ++b) {
        rows[b] = batch-1-b;
        if (masked && b == batch-1) valid[b] = width > 1 ? width-1 : 0;
        for (int p = 0; p < pages; ++p) tables[rows[b]*pages+p] = 1+b*pages+(pages-1-p);
        for (int t = 0; t < width; ++t) positions[b*width+t] = depth+t;
    }
    const auto count = [&](int leading) { return std::size_t(leading)*64*heads*physical; };
    std::vector<std::uint8_t> keys(count(128), 0xa5), values(count(128), 0xa5);
    std::vector<std::uint16_t> ks(count(4),0x3555), vs(count(8),0x3555);
    std::vector<float> k(std::size_t(256)*heads*width*batch), v(k.size());
    std::vector<float> q(std::size_t(256)*qheads*width*batch);
    fill_uniform(q, 1301+width+depth, -1.0f, 1.0f); round_to_bf16(q);
    std::vector<rk4_reference::Row> key_reference(std::size_t(batch)*live*heads), value_reference(key_reference.size());
    const auto at = [&](int leading, int b, int t, int h, int d) {
        return std::size_t(leading)*(64*(h+heads*tables[rows[b]*pages+t/64])+t%64)+d;
    };
    for (int b = 0; b < batch; ++b) for (int t = 0; t < live; ++t) for (int h = 0; h < heads; ++h) {
        std::array<double,256> x{}, y{};
        for (int d = 0; d < 256; ++d) {
            x[d] = ((17*d+3*t+11*h+7*b)%31-15)/16.0;
            y[d] = ((13*d+7*t+5*h+3*b)%29-14)/16.0;
            if (t >= depth) {
                const auto i = d+256ULL*(h+heads*(t-depth+width*b));
                k[i]=x[d]; v[i]=y[d];
            }
        }
        const auto index = h+heads*(t+live*b);
        key_reference[index]=rk4_reference::encode(x,true);
        value_reference[index]=rk4_reference::encode(y,false);
        // New rows are intentionally unwritten; fused append must own them.
        if (t >= depth) continue;
        for (int d = 0; d < 128; ++d) {
            keys[at(128,b,t,h,d)] = key_reference[index].codes[d];
            values[at(128,b,t,h,d)] = value_reference[index].codes[d];
        }
        for (int d = 0; d < 4; ++d) ks[at(4,b,t,h,d)] = key_reference[index].scales[d];
        for (int d = 0; d < 8; ++d) vs[at(8,b,t,h,d)] = value_reference[index].scales[d];
    }
    auto dk=to_device_bf16(k), dv=to_device_bf16(v), dq=to_device_bf16(q);
    auto dp=to_device(positions), dt=to_device(tables), dr=to_device(rows), dc=to_device(valid);
    GuardedDeviceBuffer ck(keys.size()), cv(values.size()), sk(ks.size()*2), sv(vs.size()*2), output(q.size()*2);
    ck.copy_from_host(keys.data(),keys.size()); cv.copy_from_host(values.data(),values.size());
    sk.copy_from_host(ks.data(),ks.size()*2); sv.copy_from_host(vs.data(),vs.size()*2); output.fill(0xa5);
    PagedKVBatchLayerView cache{
        Tensor(ck.span(),DType::U8,{128,64,heads,physical}),
        Tensor(cv.span(),DType::U8,{128,64,heads,physical}),
        Tensor(sk.span(),DType::FP16,{4,64,heads,physical}),
        Tensor(sv.span(),DType::FP16,{8,64,heads,physical}),
        Tensor(dt.span(),DType::I32,{pages,batch}),256,heads,KvCacheStorage::Rk4V4};
    Tensor tq(dq.span(),DType::BF16,{256,qheads,width,batch});
    Tensor tk(dk.span(),DType::BF16,{256,heads,width,batch}), tv(dv.span(),DType::BF16,{256,heads,width,batch});
    Tensor tp(dp.span(),DType::I32,{width,batch}), tr(dr.span(),DType::I32,{batch});
    Tensor tc = masked ? Tensor(dc.span(),DType::I32,{batch}) : Tensor{};
    Tensor out(output.span(),DType::BF16,{256,qheads,width,batch});
    const ops::AttentionHeadGeometry geometry{256,qheads,heads};
    const ops::CausalAttentionExecutionEnvelope envelope{1,static_cast<unsigned>(live)};
    const auto bytes = ops::causal_softmax_attention_workspace_capacity_bytes(geometry,KvCacheStorage::Rk4V4,envelope,batch,width,width,execution.multiprocessor_count());
    DeviceBuffer scratch(device,bytes); MemoryArena arena(scratch.span());
    const auto invoke = [&] { ops::causal_softmax_attention(tq,tk,tv,tp,tc,tr,geometry,0.0625f,cache,envelope,arena,out,execution); };
    invoke(); cuda_synchronize();
    if (graph) {
        DecodeGraphDefinition definition;
        DecodeGraphExecutable executable;
        definition.capture(execution.stream(), invoke);
        executable.instantiate(definition);
        executable.launch(execution.stream());
        cuda_synchronize();
    }
    int failures=0;
    for (int b = 0; b < batch; ++b) for (int t = depth; t < depth+valid[b]; ++t) for (int h = 0; h < heads; ++h) {
        const auto index=h+heads*(t+live*b);
        for (int d=0;d<128;++d) { keys[at(128,b,t,h,d)]=key_reference[index].codes[d]; values[at(128,b,t,h,d)]=value_reference[index].codes[d]; }
        for (int d=0;d<4;++d) ks[at(4,b,t,h,d)]=key_reference[index].scales[d];
        for (int d=0;d<8;++d) vs[at(8,b,t,h,d)]=value_reference[index].scales[d];
    }
    failures += verify_exact("rk4 key bytes",from_device<std::uint8_t>(ck.data(),keys.size()),keys);
    failures += verify_exact("rk4 value bytes",from_device<std::uint8_t>(cv.data(),values.size()),values);
    failures += verify_exact("rk4 key scales",from_device<std::uint16_t>(sk.data(),ks.size()),ks);
    failures += verify_exact("rk4 value scales",from_device<std::uint16_t>(sv.data(),vs.size()),vs);
    const auto got=from_device_bf16(output.data(),q.size());
    if (batch == 1) {
        PagedKVLayerView single{cache.k_pages,cache.v_pages,cache.k_scale_pages,
                               cache.v_scale_pages,cache.block_tables,256,heads,KvCacheStorage::Rk4V4};
        ops::kv_cache_append(tk,tv,tp,single,execution);
        ops::causal_softmax_attention_cached(tq,tp,geometry,0.0625f,single,envelope,arena,out,execution);
        cuda_synchronize();
        failures += verify_exact("rk4 standalone/fused keys",from_device<std::uint8_t>(ck.data(),keys.size()),keys);
        failures += verify_exact("rk4 standalone/fused values",from_device<std::uint8_t>(cv.data(),values.size()),values);
        failures += verify_exact("rk4 standalone/fused key scales",from_device<std::uint16_t>(sk.data(),ks.size()),ks);
        failures += verify_exact("rk4 standalone/fused value scales",from_device<std::uint16_t>(sv.data(),vs.size()),vs);
        failures += verify_exact("rk4 cached/append attention",from_device_bf16(output.data(),q.size()),got);
    }
    for (int b=0;b<batch;++b) for (const int token : {0,width/2,width-1}) {
        std::vector<double> expected(256*qheads), actual(expected.size());
        for (int h=0;h<qheads;++h) {
            std::array<double,256> query{};
            for(int d=0;d<256;++d) query[d]=q[d+256ULL*(h+qheads*(token+width*b))];
            rk4_reference::rotate(query);
            std::vector<double> scores(depth+token+1);
            double maximum=-INFINITY, sum=0;
            if(token<valid[b]) {
                for(int t=0;t<=depth+token;++t) {
                    const auto& key=key_reference[h/(qheads/heads)+heads*(t+live*b)].decoded;
                    scores[t]=std::inner_product(query.begin(),query.end(),key.begin(),0.0)/16.0;
                    maximum=std::max(maximum,scores[t]);
                }
                for(auto& score:scores) { score=std::exp(score-maximum); sum+=score; }
                for(int t=0;t<=depth+token;++t) {
                    const auto& value=value_reference[h/(qheads/heads)+heads*(t+live*b)].decoded;
                    for(int d=0;d<256;++d) expected[h*256+d]+=scores[t]/sum*value[d];
                }
            }
            for(int d=0;d<256;++d) actual[h*256+d]=got[d+256ULL*(h+qheads*(token+width*b))];
        }
        failures += verify_reduction("rk4 FP64 attention",actual,expected,criterion);
    }
    failures += ck.verify_guards("rk4 keys")+cv.verify_guards("rk4 values")+sk.verify_guards("rk4 key scales")+sv.verify_guards("rk4 value scales")+output.verify_guards("rk4 output");
    std::cout << "rk4 h="<<heads<<" W="<<width<<" depth="<<depth<<" B="<<batch<<" masked="<<masked<<" failures="<<failures<<'\n';
    return failures;
}
}
int main() {
    if(cuda_unavailable()) return 77;
    int failures=0;
    for(int heads:{2,4}) {
        for(int width:{1,4,8,9,16,129,257,1024,2048}) failures+=run(heads,width,63,1,false,false);
        for(int width:{1,4,8,16}) for(bool masked:{false,true}) failures+=run(heads,width,129,2,masked,true);
        for(int depth:{0,32768,65536,122880,262143}) failures+=run(heads,1,depth,1,false,false);
    }
    return failures ? 1 : 0;
}
