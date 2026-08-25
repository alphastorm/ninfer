# NInfer 资源调度与上下文缓存架构

**文档状态：** 当前已实现架构
**目标模型：** 当前注册的 Qwen3.6/Qwen3.8 family variants
**目标硬件：** 单张 NVIDIA RTX 5090
**所属层级：** Engine / ResourceManager / Program
**目标场景：** 本地 Agent、小规模并发、长上下文、MTP/DFlash speculative decoding

NInfer 面向单张 RTX 5090，并围绕当前注册的混合 GDN/Full Attention variants 深度特化。模型 state 是固定大小的完整 image，而 Main/Backend KV 随上下文线性增长；在权重、最大上下文、workspace 和 CUDA Graph 已占用大量显存的前提下，VRAM 中只能容纳有限的额外 state 和 KV 缓存，因此本架构将有界Host memory定义为可配置的正式后备存储层。

本文是当前资源调度与上下文缓存的正式架构，不记录备选设计、测试清单或分阶段实施步骤。顶层请求生命周期与调度边界见[Engine架构](engine-architecture.md)，物理KV容器与consumer contract见[Paged KV Context Store](paged-kv-cache.md)。

---

## 1. 系统定位

本设计不是一个独立的 prefix cache，也不是位于 Scheduler 之后的第二套调度器。

它是Engine内部由`ResourceManager`与`Program`共同闭合的资源管理架构。ResourceManager负责：

* 在 Scheduler 已选中的请求上寻找最佳恢复起点；
* 管理active lane及state/KV reservation ledger；
* 选择Device/Host state/KV replicas的保留、下沉和逐出意图；
* 创建、保留、降级和逐出 checkpoints；
* 组织Program将逻辑上可恢复的continuation materialize为可执行active sequence；
* 保证 active request 的完整生命周期资源。

整体职责关系为：

```text
Scheduler
    决定哪个请求当前有资格运行
        │
        ▼
ResourceManager
    决定使用哪个 continuation/checkpoint
    决定恢复、保留、下沉或逐出哪些逻辑资源
        │
        ▼
Program
    精确验证 prefix
    执行 state/KV 的物理变换
    执行 prefill / decode / speculative commit
```

Prefix caching 是 ResourceManager 复用已有 continuation、降低重复 prefill 成本的一种策略。

ResourceManager 不改变 FIFO、protected-head、backfill 等请求公平性。一个后到请求不能仅因为 prefix hit 更长，就越过 Scheduler 已经选择的请求。

---

## 2. 核心设计理念

### 2.1 State 决定前缀是否可恢复

对于 Qwen3.8-27B：

* Full Attention KV 可以截断到任意精确前缀；
* GDN state 不能从当前状态无损回退；
* 只有在某个 frontier 存在完整 state checkpoint，才能从该位置继续计算；
* 只有 KV 而没有匹配 state，不构成可用命中。

因此：

> **Checkpoint 的 state 是前缀可恢复性的证明，KV 是该 checkpoint 恢复时所需的分页前缀存储。**

这一点是整个架构的第一原则。

---

### 2.2 State 与 KV 不同构

State 和 KV 具有本质不同的资源语义。

#### State

* 一个 state image 对应一个精确 frontier；
* 必须作为完整 image 保存和恢复；
* 不存在“半个 state”；
* 每个 checkpoint 需要独立的 state image；
* Device/Host 之间以完整 image 为迁移单位。

#### KV

* 表示序列的分页前缀；
* 可以按 page 或 logical extent 分别驻留；
* 可以只有一部分在 Device、另一部分在 Host；
* 多个 checkpoints 可以引用同一 KV address space 的不同前缀；
* 多个 shared prefixes 或 active branches 可以共享 immutable pages。

因此系统必须允许：

```text
State: Device
KV:    部分 Device、部分 Host
```

以及：

```text
State: Host
KV:    大部分 Device、少量 Host
```

而不能把 state 与 KV 强制绑定成一个整体迁移对象。早期设计已经提出 state 与 KV 独立驻留的方向，最终架构将其提升为核心数据模型。

---

### 2.3 Continuation 是逻辑所有权单元

`Continuation` 表示一条线性序列历史，而不是一个必须整体迁移的物理 bundle。

一个 private continuation 包含：

```text
Continuation
├── target identity / token ledger
├── Main KV address space
├── Backend KV address space
├── Endpoint checkpoint
├── Optional typed rewrite checkpoint
└── Optional sparse long anchors
```

其中：

* Endpoint state 对应当前完整 continuation frontier；
* Rewrite state 对应同一序列历史中的较早 frontier；
* Endpoint 和 rewrite 共享同一条 KV address space；
* 每个 checkpoint 只引用该 address space 的一个前缀。

引入 `Continuation` 的必要性在于表达：

* endpoint 和 rewrite 属于同一条历史；
* 它们共享哪些 KV；
* 恢复 rewrite 后，哪些 endpoint suffix 应被释放；
* 删除 endpoint 后，continuation 是否仍可从 rewrite 恢复；
* 资源逐出时，可以降低 continuation 的可恢复层级，而不是只能整体删除。

`Continuation` 是逻辑归属对象，不是物理迁移单位。

---

### 2.4 Checkpoint 是精确 state 位置

一个 checkpoint 表示：

```text
某个 target prefix identity
在某个精确 token frontier
对应的一幅完整 StateImage
以及恢复该 frontier 所需的各 typed KV prefix
```

概念结构：

```cpp
struct Checkpoint {
    CheckpointId id;
    CheckpointKind kind;
    CheckpointScope scope;

    uint32_t token_frontier;
    TargetKVRequirement kv_requirement;

    StateImageHandle state;
    PrefixIdentity identity;

    Optional<CheckpointId> fallback;
};
```

`TargetKVRequirement` 由 Program 定义，不要求 Main 与 Backend 使用相同的数值 frontier。例如 speculative backend KV 可以具有 target-defined 的 off-by-one 关系。它是 Program 提供给 ResourceManager 的 opaque requirement 与资源摘要；当前连续前缀实现可以使用 page count 和尾页 coverage 表达，但该具体布局不是跨模块 contract。

`StateImageHandle` 是 Program 铸造并校验 generation 的 opaque capability。它引用的是当前 Variant 在该 checkpoint frontier 继续执行所需的完整、封闭 state payload，包括该 Variant 实际拥有的 recurrent state、convolution history、tail/rewrite hidden state及 Backend fixed state。ResourceManager 不读取 raw tensor，也不自行解释 state layout。

Checkpoint 可以是：

```text
SessionEndpoint
TurnClosure
ResponseReplay
SharedStablePrefix
LongAnchor
```

其中 `TurnClosure` 和 `ResponseReplay` 是 Agent fallback 的正式类型，不再使用笼统的 “input anchor” 作为底层语义。

---

### 2.5 Active 状态可变，Checkpoint 不可变

Active sequence 的 state 和 KV 会持续推进。

一旦 checkpoint 被发布：

* state image 不允许再被原地修改；
* checkpoint identity 和 frontier 不允许变化；
* checkpoint 所需 KV prefix 的已有内容不可被覆盖；
* residency 可以变化，但逻辑内容不变。

因此 state 操作分为：

```text
InPlace
Move
Fork
Freeze
Snapshot
Restore
```

---

### 2.6 存储对象具有唯一身份

Checkpoint、continuation 和 active branch 可以引用同一个 logical KV page，因此 page metadata 不能内嵌复制到每个catalog entry。每个 Program 对每个 typed KV pool 维护唯一的 logical page store，并拥有KV address-space objects：

```text
LogicalKVPageStore[typed pool]
    LogicalKVPageHandle → logical page state

KVAddressSpaceHandle → KVAddressSpace
    └── ordered LogicalKVPageHandle[]
```

ContinuationCatalog保存`KVAddressSpaceHandle`及Program返回的revisioned policy summary，不复制address-space membership或replica metadata。ResourceManager通过opaque logical page handles表达victim/retention intent，只有Program能够修改address space及其references。

`LogicalKVPageHandle` 是Program铸造的opaque capability，其identity只需在所属Program lifetime和typed pool内唯一；架构不要求ID永久增长或跨Engine复用。架构区分两类版本：

```text
capability generation
    拒绝对象槽位复用后的 stale handle

content epoch + committed coverage
    判断 Device/Host replica 是否属于当前逻辑内容，
    以及它能覆盖到该 page 的哪个 committed column
```

Program 可以通过更新 content epoch，也可以通过铸造新的 logical page handle 表达 destructive divergence；具体表示不属于 ResourceManager contract。无论采用哪种表示，都必须保证 stale replica 不会被重新解释为当前内容。

正常 append 不改变既有 committed prefix 的 content epoch，只在 Program commit 后推进 committed coverage。Speculative 或尚未提交的 bytes 可以物理存在，但不能进入 canonical committed coverage，也不能被 checkpoint引用。

同一物理 page、state slot 或 Host extent 无论被多少逻辑对象引用，在容量账本中都只计算一次。引用关系、replica coverage 和物理 generation 的唯一事实来源是 Program。

---

## 3. 顶层对象模型

### 3.1 PrivateContinuation

```cpp
struct PrivateContinuation {
    ContinuationId id;
    Revision revision;

    Optional<SessionKey> session;

    TargetPrefixIdentity identity;
    TokenLedger ledger;

    KVAddressSpaceHandle main_kv;
    Optional<KVAddressSpaceHandle> backend_kv;

    Checkpoint endpoint;
    Optional<Checkpoint> rewrite;
    SmallVector<Checkpoint> long_anchors;

    ContinuationPolicy policy;
};
```

特点：

* 同一时间只能被一个 active request claim；
* 恢复通常是 destructive Move；
* endpoint 和 rewrite 共用 KV address spaces；
* rewrite restore 会截断或删除 endpoint suffix；
* session 默认只保留最新 endpoint 和最新 typed rewrite。

---

### 3.2 SharedPrefix

```cpp
struct SharedPrefix {
    SharedPrefixId id;
    Revision revision;

    Checkpoint checkpoint;

    KVAddressSpaceHandle main_kv;
    Optional<KVAddressSpaceHandle> backend_kv;

    SharedPrefixPolicy policy;
};
```

特点：

* 完全不可变；
* 可被多个 requests 同时命中；
* state 恢复使用 Fork；
* full KV pages 可以共享；
* 非 page-aligned tail 对每个 branch 建立 private copy；
* 不被某个 request destructive consume。

Private continuation 和 shared prefix 共享底层 state/KV storage abstraction，但具有不同 ownership 语义。

---

### 3.3 ActiveSequence

Active sequence 是已经 materialize 到 Device 的可执行上下文：

```cpp
struct ActiveSequence {
    LaneId lane;

    SequenceHandle sequence;

    FullLifetimeReservation reservation;

    uint32_t committed_frontier;
    Optional<CheckpointId> restored_from;
};
```

Active sequence 必须满足：

* state binding可执行：InPlace/Move/Restore具有完整Device state，Fork具有完整immutable read source及独占write destination；
* 当前执行所需的所有 KV pages 在 Device；
* block tables 已安装；
* 完整 future growth entitlement 已保留；
* 所有资源在 active lifetime 内 pinned。

`SequenceHandle` 是 Program 对完整物理 active binding 的 capability。RequestRecord 持有该 capability，ResourceManager只维护对应的逻辑 lane/entitlement ledger，不复制其物理 slot、page 或 block-table事实。

Program 内部的 state binding 为：

```cpp
struct ActiveStateBinding {
    DeviceStateSlotHandle read;
    DeviceStateSlotHandle write;
    bool fork_pending;
};
```

`InPlace`、`Move` 和已经完成的 `Restore` 使用同一个 read/write slot。`Fork` 在 source checkpoint 与 active destination之间建立不同的 read/write slot，并在 Program commit确认 destination已经成为完整、可继续执行的 committed state image之前保持 `fork_pending`。Engine 和 ResourceManager不根据某个 kernel launch或某个 GDN字段自行推断 Fork完成。

---

### 3.4 PrivateContinuation 生命周期

Private continuation 具有唯一的逻辑 ownership lifecycle：

```text
Catalogued
    ↓ claim by MaterializationTransaction
Claimed
    ├── abort before publish → Catalogued
    └── publish             → Active
                                  ├── normal finish → Catalogued(new continuation)
                                  └── cancel/error  → Dropped
```

`Claimed` continuation从普通 candidate set中隐藏，不能被第二个 request复用。相同 `SessionKey` 的并发请求仍可从 shared prefix、其他合法 checkpoint或 root开始；`SessionKey` 不是 session mutex。

Publish 后，active ownership包含该 private continuation的完整 capability：KV address spaces、仍然保留的 rewrite/long anchors、identity、ledger与lineage metadata。正常 finish从完整 committed active state Freeze新的 endpoint并原子返回 catalog。Cancellation或执行错误默认不发布新 checkpoint；架构也不保证 destructive Move之前的旧 endpoint仍然存在。

---

## 4. State 资源架构

### 4.1 统一 DeviceStatePool

最终 Device state 容量为：

\[
N_{\text{device state}} = C + H
\]

其中：

* (C)：最大 active concurrency；
* (H)：满并发时仍保证存在的 Device checkpoint capacity。

所有 Device slots 完全同构，不固定划分为：

```text
current[C]
rewrite[C]
cache[H]
```

也不与 lane 永久绑定。

一个 slot 可以处于：

```text
FREE
ACTIVE_MUTABLE
CHECKPOINT_IMMUTABLE
TRANSFER_SOURCE
TRANSFER_TARGET
```

必须满足：

\[
A_s + D_s + I_s + T_s \le C + H
\]

其中所有项都按 unique physical slots 计数且互不重复：

* \(A_s\)：已经发布的active write/destination slots；
* \(D_s\)：已经预留、尚未发布且尚未被其他项计数的新destination slots；
* \(I_s\)：immutable Device checkpoint replicas，包括Fork pending source；
* \(T_s\)：不属于前三项的Device state transfer destination reservations。

Private Move把已有source slot从 \(I_s\) 原子重分类到 \(A_s\)，不另计一个 \(D_s\)。Fork需要独立destination，publish前进入 \(D_s\)，publish后转入 \(A_s\)，其immutable source继续进入 \(I_s\)。

且：

令 \(Q_a\) 为已经发布的active requests，\(Q_r\) 为已经reserve但尚未publish的request activations，则控制面并发满足：

\[
Q_a + Q_r \le C
\]

Cache 可以借用当前未使用的 active capacity。例如：

```text
C = 2
H = 2
```

允许：

```text
2 active + 2 checkpoints
1 active + 3 checkpoints
0 active + 4 checkpoints
```

令 `free` 表示尚未被任何transaction预留的slots，`reclaimable`表示可立即释放且没有source、active或transfer pin的checkpoint slots，则任何admission/transfer boundary都必须满足：

\[
\text{free} + \text{reclaimable}
\ge \max(0, C-Q_a-Q_r)
\]

这等价于不可立即回收的非 active Device state占用不超过 \(H\)。Cache可以借用空闲 active capacity，但后台 transfer、source pin和未完成 Fork不能破坏达到 \(C\) 路 active concurrency所需的容量保证。

统一 `C+H` 的资源模型及独立 Host state pool 是本架构的基础。

---

### 4.2 HostStatePool

每个checkpoint的`StateImageHandle`引用Program-owned `StateImageStore`中的一幅完整immutable image。该logical image可以同时具有Device和Host replica；两份replica共享同一逻辑身份，但分别占用一个Device slot和一个Host slot。State不存在partial coverage，任一已发布replica都必须包含当前Variant要求的完整payload。

ResourceManager只保存handle、residency/footprint summary及其revision。Replica location、slot generation和copy completion由Program唯一维护。

Host state 容量为：

\[
N_{\text{host state}} = R
\]

同时满足：

\[
N_{\text{published Host replicas}} + N_{\text{reserved Host targets}} \le R
\]

HostStatePool 以完整 StateImage 为单位。

它保存：

* cold endpoint state；
* typed rewrite state；
* shared stable prefix state；
* optional long anchor；
* Device state 的 backup replica。

需要注意：

> (R) 表示 state images 的数量，不是 session 数量。

一个 Agent continuation 同时保存 endpoint 和 rewrite 时，需要两个 Host state images。

HostStatePool 提供：

```text
allocate / release
Device → Host
Host → Device
pin / unpin
replica publication
generation validation
```

它不理解 Agent、session 或 cache priority。

---

### 4.3 Lane 与 state slot 解耦

Lane 继续表示：

* request control identity；
* Scheduler membership；
* compact batch row selection；
* Program capability epoch。

State slot 表示：

* target fixed continuation image。

映射可以是：

```text
lane 0 → state slot 4
lane 1 → state slot 1
```

而 checkpoint 可以位于：

```text
checkpoint A → state slot 0
checkpoint B → state slot 3
```

Program 的 state operators 接收每个 batch row 的：

```text
state_src_slot[B]
state_dst_slot[B]
```

正常 decode：

```text
src == dst
```

从 checkpoint 分叉：

```text
src != dst
```

不同 rows 可以共享同一个 immutable source，但 destination 必须互不相同。

这些 selectors是 Program内部物理 binding的一部分，不进入ResourceManager的可变状态。所有会推进完整 target state的路径，包括 speculative replay/fold，均使用同一 src/dst contract。

---

## 5. State 操作语义

### 5.1 InPlace

普通 active 推进：

```text
read  S0
write S0
```

这是 decode 热路径的默认形式。

架构扩展不应让普通 decode 被迫使用额外 state copy。

---

### 5.2 Move

Private checkpoint 被单一 request 消费时：

```text
CHECKPOINT S0
    ↓ ownership transfer
ACTIVE S0
```

无需 D2D copy。

适用于：

* private endpoint exact hit；
* private rewrite checkpoint restore；
* private continuation重新进入 active execution。

---

### 5.3 Fork

Shared 或需要继续保留的 immutable checkpoint：

```text
read  checkpoint S0
write active     S1
```

Program launch之后，destination仍是 provisional state。只有 Program commit确认该 destination已经成为完整、可继续执行的 committed active state image后，Fork才完成：

```text
S0 仍是 checkpoint
S1 成为 active state
read = write = S1
fork_pending = false
release source pin
```

如果 exact-hit finalization没有建立完整 destination state，Fork继续 pending到后续能完成该状态转换的 commit。Program在 commit result中显式报告 Fork是否已经完成；Engine不以“是否推进某个 GDN字段”代替这一判断。

Fork完成前的 cancellation释放 destination reservation和source pin，不修改catalogued checkpoint。

多个 requests 可以同时：

```text
row 0: S0 → S1
row 1: S0 → S2
row 2: S0 → S3
```

Fork 是 shared stable prefix fan-out 的基础。

---

### 5.4 Freeze

当 active request 到达需要保存的 frontier：

```text
ACTIVE S0
    ↓ reclassify
CHECKPOINT S0
```

如果请求还需要继续计算，则分配新 active destination：

```text
next unit:
    read  S0
    write S1
```

即：

```text
Freeze current
→ Fork next active
```

这避免了创建 checkpoint 时完整复制约 150 MiB state。

---

### 5.5 Snapshot

当 checkpoint 应进入 Host：

```text
Device checkpoint
    ↓ D2H
Host checkpoint
```

如果 checkpoint 仍要作为 active source继续执行，可以先 Freeze + Fork，再异步将 immutable source下沉 Host。

如果没有额外 Device slot，则必须在 current state 继续修改前完成 Host snapshot。

---

### 5.6 Restore

Host checkpoint恢复到 Device：

```text
Host state
    ↓ H2D
Device state slot
```

Private state恢复后可以直接成为 active。

Shared Host checkpoint可以：

* 直接恢复到 active destination；
* 或建立一个 Device checkpoint replica，供后续多次 Fork。

---

## 6. Agent state 轮换

假设某个 private continuation当前拥有：

```text
endpoint state = S0
rewrite state  = S1
```

下一轮 endpoint命中：

```text
S0: CHECKPOINT → ACTIVE
S1: 保留旧 rewrite
```

到达新的 rewrite frontier：

1. 旧 rewrite S1准备被替换；
2. 当前 active S0被 Freeze 为新 rewrite；
3. S1被复用为新的 active destination。

```text
new rewrite = S0
new active  = S1
```

请求结束：

```text
S1: ACTIVE → new endpoint
```

最终仍为：

```text
rewrite  = S0
endpoint = S1
```

两个 slots持续交换角色：

* 无固定 current/rewrite half；
* 无完整 state snapshot copy；
* 不需要第三个临时 state image。

---

## 7. KV 地址空间

### 7.1 Typed KV pools

当前 Variant 提供一个 Main KV pool，并在其执行语义需要时提供一个独立 Backend KV pool：

```text
Main KV pool
Optional Backend KV pool
```

每个 pool具有：

* 独立 page namespace；
* 独立 logical frontier；
* 独立 capacity；
* 独立 Host residency；
* 独立 materialization requirement。

策略层可以将它们视为同一个 continuation的一部分，但资源 accounting 必须是向量：

```cpp
struct KVResourceVector {
    uint32_t main_pages;
    uint32_t backend_pages;
};
```

一个 pool有空闲 pages，不能替代另一个 pool的容量。

---

### 7.2 KVAddressSpace

每个continuation对每个typed pool持有一个Program-owned logical address-space capability：

```text
KVAddressSpace
├── typed pool identity
├── logical frontier
└── ordered LogicalKVPageHandle[]
```

Catalog不内嵌address-space membership、Device/Host replica、refcount或transfer state。Program-owned `KVAddressSpace`与`LogicalKVPageStore`是这些事实的唯一authority。ResourceManager使用Program发布的revisioned summaries做candidate/victim选择。每个logical page在概念上维护：

```text
opaque object identity + capability generation
canonical content epoch + committed valid coverage
logical references and write protection
optional Device replica
optional Host replica
transfer state
```

每个 replica携带其复制时的content epoch及自身committed coverage。Replica能够满足 page前 \(n\) 个columns，当且仅当：

\[
\text{replica.content_epoch}
= \text{page.current_content_epoch}
\quad\land\quad
\text{replica.committed_coverage} \ge n
\]

Program不必将这些概念逐字段实现；它也可以通过创建新 logical page object表达新content epoch。跨模块contract只要求object identity、当前内容和replica coverage能够被唯一判定。

一个 logical page可以具有以下 residency：

```text
DEVICE_ONLY
HOST_ONLY
DEVICE_AND_HOST
MIGRATING_TO_HOST
MIGRATING_TO_DEVICE
```

Transfer发布时必须再次核对source capability、content epoch和coverage。过期transfer result不能覆盖当前replica事实。

#### Private write protection

Page写入合法性不由 `reference_count > 1` 或一个固定的mutable/immutable enum单独决定。Program依据writer cardinality、跨address-space sharing和仍存checkpoint的protected frontier执行：

```text
private lineage
    最多一个writer
    可以向protected frontier之后append
    不得覆盖任一surviving checkpoint所需的prefix

shared across address spaces
    immutable
    branch写入前建立private tail/page
```

Private destructive rewrite只有在旧endpoint已经被claim/consume，并且所有仍保留checkpoint都不需要被覆盖suffix时才可原地截断。Program随后重新认证仍一致的prefix coverage，并使旧suffix replica失效；若这些条件不成立，则必须创建新的private logical page，而不能修改shared object。

---

### 7.3 Checkpoint 有效性

对 checkpoint \(c\)，Program 定义每个 pool所需的 logical pages及每页coverage：

\[
P_s(c)
\]

Checkpoint逻辑有效，当且仅当：

1. state image至少存在一份完整 replica；
2. 每个 required KV page至少存在一份epoch一致、coverage充足的replica。

即：

\[
\forall s,\forall p\in P_s(c):
\operatorname{valid\_device}(p,c)\lor\operatorname{valid\_host}(p,c)
\]

Checkpoint不要求所有资源位于同一层级。

---

### 7.4 Device-ready

Checkpoint只有在：

* state位于 Device；
* 每个 required KV page在Device有epoch一致、coverage充足的replica；
* active future reservation可满足；

时才是 Device-ready。

因此：

```text
有效 checkpoint
≠
可立即执行 checkpoint
```

ResourceManager 的主要职责之一，就是将有效 checkpoint materialize 为 ActiveSequence。

---

## 8. Host KV 存储

### 8.1 使用 packed logical-order extents

Host KV 不保存原 device physical page layout。

一个 Host extent以 logical page顺序存储：

```cpp
struct HostKVExtent {
    PoolId pool;

    uint32_t logical_begin;
    uint32_t logical_page_count;

    HostAllocation allocation;
    HostPlaneLayout layout;
};
```

例如：

```text
Extent:
    logical pages [3500, 3600)
```

Host payload中：

```text
Host page 0 = logical page 3500
Host page 1 = logical page 3501
...
```

Device physical page IDs只是当前 allocator的临时结果，不进入 Host canonical representation。

---

### 8.2 为什么不直接 memcpy Device physical layout

直接复制整个 physical KV slab存在以下问题：

* slab包含多个 requests的数据；
* continuation只拥有其中少量 pages；
* physical page namespace可能高度稀疏；
* Host空间会与整个 pool而非实际 continuation长度绑定；
* 原 physical page IDs恢复时通常已被其他请求占用；
* prefix-only restore需要额外解释 physical holes。

即使只按原 physical ID保存所属 pages，也会产生：

* 稀疏 Host layout；
* 对临时 page IDs的无意义依赖；
* 复杂的 prefix截断；
* 较差的 Host空间利用率。

Packed logical order的优势是：

* Host bytes与实际 page数成正比；
* 恢复时可使用任意新 physical pages；
* 天然支持只恢复某个 checkpoint frontier所需的 prefix；
* 适合 variable-size allocation和transfer coalescing；
* 逻辑布局不受 Device fragmentation影响。

传输时仍应识别连续 physical runs，并将连续 pages合并成较大的 D2H/H2D copy。因此：

```text
Host representation：logical-order packed
Transfer execution：physical-run coalesced
```

两者并不冲突。

---

### 8.3 Variable-size Host KV arena

Host KV 不应使用：

```text
N × max-sequence-sized slabs
```

而应使用有界的 variable-size extent allocator：

```text
HostKVArena
├── size-class / extent allocator
├── packed logical extents
└── bounded capacity
```

短 continuation只占实际 mapped pages所需的 Host bytes。

State 仍然使用 fixed image slots，因为 state与 KV 的资源形态不同。

### 8.4 Process 与 platform boundary

`HostStatePool`、`HostKVArena`、`StateImageStore`和`HostKVExtentStore`只保存
Program-owned、in-process的Host replica。它们不定义disk serialization、journal、
cross-process restore或第二种product artifact。

Microsoft DirectStorage不适用于当前`sm_120a` WSL runtime。该API依赖`_WIN32`、
`dstorage.h`、D3D12/DXGI、Win32 shared handles及CUDA-D3D12 external-memory/semaphore
interop；WSL target编译并执行Linux binary。旧disk-cache实现所依赖的`DiskStateCache`和
`snapshot_turn_checkpoint_to_disk`也已由本章的StateImage与packed logical-order Host KV
架构取代。不得通过恢复旧文件、compatibility alias或静默fallback伪造可用的DirectStorage
route。

如果cross-process persistence未来进入product scope，必须直接以canonical StateImage和
packed logical-order Host KV representation定义disk format、lifecycle与failure semantics，
并选择目标runtime原生支持的storage backend。在此之前，DirectStorage对WSL route为
**non-applicable**，不是未验证但可启用的runtime capability。

---

## 9. 部分 KV 驻留

部分 KV offload是核心能力，不是特殊路径。

假设：

```text
Device KV 总容量：256K
resident continuation：230K
当前 free：26K
新请求需要：32K
缺口：6K
```

如果 230K属于 inactive continuation，只需逐出 page-rounded 后足够覆盖 6K 的 KV：

```text
[0,224K)      Device
[224K,230K)   Host
```

而不需要把整个 230K continuation复制到 Host。

该 continuation仍然有效：

```text
State checkpoint：存在
KV [0,224K)：Device
KV [224K,230K)：Host
```

未来恢复 endpoint时，只需恢复缺失的约 6K尾部。

这也是 state 与 KV 必须独立 residency 的直接原因。

---

### 9.1 默认优先逐出 private suffix

假设：

```text
rewrite frontier  = 180K
endpoint frontier = 230K
```

优先逐出：

```text
[224K,230K)
```

可以保证：

* rewrite checkpoint仍完全 Device-ready；
* endpoint只缺少短 Host tail；
* endpoint mismatch时无需恢复 Host KV；
* endpoint exact hit时只需恢复少量尾部。

对于同一线性 continuation，越靠前的 pages通常被更多 checkpoints依赖。

可将某 page的边际价值近似为：

\[
V(p)=\sum_{c:p\in P(c)} W(c)
\]

其中 (W(c)) 是 checkpoint的复用价值。

因此 trailing private pages通常是最低价值的 Device replicas。

---

### 9.2 不强制只能逐出尾部

ResourceManager 可以逐出任意 logical extents。

优先级通常为：

1. 已有 Host replica、可直接删除 Device replica的低价值 pages；
2. endpoint在 rewrite frontier之后的 private suffix；
3. cold private continuation的 suffix；
4. 低 fan-out shared pages；
5. 高 fan-out shared stable prefix pages最后处理。

如果中间某段已有 Host backup，而尾部尚无 Host backup，直接释放该中间 Device replica可能更便宜。

---

### 9.3 Main 和 Backend 分别逐出

Device缺口是资源向量：

```text
main_deficit
backend_deficit
```

ResourceManager为不同 pools分别选择 extents。

一个 continuation可以处于：

```text
Main KV：80% Device，20% Host
Backend KV：100% Device
```

或者相反。

Target committed semantics仍由 Program保证，residency不需要相同。

---

## 10. Private KV 与 Shared KV 的不同语义

### 10.1 Private continuation

Private continuation同一时间只能有一个 active owner。

Endpoint、rewrite和active continuation属于同一条单分支历史。

因此：

* full pages可以直接共用同一 KV address space；
* checkpoint与后续 active可以共享非 page-aligned tail page；
* active只会向 checkpoint frontier之后的新 offsets追加；
* 旧 checkpoint使用较短 valid frontier，忽略后续写入的 bytes；
* 不需要为同一 private continuation内的 append执行 tail COW。

这里的“共享”是同一private address space内部多个frontiers对protected prefix的引用，不代表存在多个writers。Program只有在commit时推进canonical coverage；speculative suffix即使已经写入Device page，也不能被旧checkpoint或新checkpoint视为committed内容。

Private rewrite restore时：

```text
restore rewrite state
→ 将 logical frontier回退到 rewrite
→ 释放或忽略 endpoint suffix
→ 继续新的单分支历史
```

回退发生在partial tail内部时，Program必须使rewrite frontier之后的旧content失效。只有没有surviving checkpoint或shared address space需要该suffix时，才允许在原logical page上切换content epoch并继续写入；否则必须创建private page，保留原page内容。

---

### 10.2 Shared prefix

Shared prefix允许多个 requests并发 Fork。

如果 shared frontier位于 page中间，不同 branches会向相同 offsets写入不同 suffix，因此必须：

```text
共享所有完整 prefix pages
为每个 branch复制一个 private tail page
```

即：

```text
immutable full pages
+ per-branch partial-tail copy
+ private suffix pages
```

只有真正存在多分支写入时才需要 tail COW。

---

## 11. Checkpoint 类型

### 11.1 Session Endpoint

表示某个 private session上一次请求结束后的完整 committed continuation。

下一请求可能：

```text
exact endpoint
endpoint + suffix
```

Private endpoint通常使用 Move恢复。

每个 session默认只保留最新 endpoint。

---

### 11.2 Typed Rewrite Checkpoint

用于调用方重新序列化模型输出时的 fallback。

正式类型包括：

```text
TurnClosure
ResponseReplay
```

可能位于 prompt 中间。

如果 endpoint不匹配，但 rewrite匹配：

```text
恢复 rewrite state
截断 KV 到 rewrite requirement
释放 endpoint suffix
prefill 新 divergence suffix
```

每个 session默认只保留最新 typed rewrite。

---

### 11.3 Shared Stable Prefix

来源包括：

* 大型 system prompt；
* 稳定 tool definitions；
* 调用方显式标记的 cacheable prefix；
* 多个 sessions共用的模板前缀。

特征：

* state不可变；
* KV不可变；
* 可反复 Fork；
* full pages共享；
* partial tail按 branch复制；
* 通常具有较高 Device residency价值。

---

### 11.4 Long Anchor

用于极长 private continuation中的稀疏中间恢复点。

只在高价值自然边界建立，例如：

* 调用方显式标记；
* 长时间暂停边界；
* 距离最近 checkpoint重建成本过高的位置。

不采用固定短间隔自动 checkpoint，也不建立 token-level radix tree。

---

## 12. Catalog

```text
ContinuationCatalog
├── SessionIndex
└── ContentPrefixIndex
```

### 12.1 SessionIndex

```text
SessionKey
    → latest PrivateContinuation
```

Private continuation内部再暴露：

```text
Endpoint candidate
Rewrite candidate
Long-anchor candidates
```

SessionIndex只维护命名continuation的latest-lineage替换与retention关系，不证明prefix匹配，也不是未命名
Agent追加命中的前提。

---

### 12.2 ContentPrefixIndex

Private endpoint、typed rewrite、long anchor和shared stable prefix使用同一套target-derived shortlist
协议。Incoming prepared prompt在一次线性遍历中生成每个token frontier的rolling digest；catalog entry
保存checkpoint创建时的静态key，例如：

```text
prefix digest
frontier
identity class
checkpoint kind / scope
```

ResourceManager只把key相同的具体checkpoint交给Program。没有匹配bucket时直接保留root候选，不能把
全catalog重新追加为“fallback candidates”。最终exact verification仍必须由Program完成；Hash相同不代表
prefix相同。

Private source的ownership由source是否已经属于命名lineage决定：命名source仅在incoming持有相同
SessionKey且允许更新SessionIndex时可以Consume/Move；incoming缺少SessionKey、使用不同SessionKey，或
禁止更新SessionIndex时都必须Retain/Fork。匿名source没有需要保护的SessionIndex owner，因此无论
incoming是否命名、是否更新SessionIndex，单分支追加都允许Consume/Move。Program仍可因long anchor或
state-image aliasing把该最低约束提升为Retain，但不能把必须Retain的命名source降为Consume。

Session identity因此只决定ownership disposition，不决定内容是否命中；`update_session_index=false`也
不表示匿名content-matched source必须保留。

---

### 12.3 Fallback relation

每个 checkpoint可以指向最近的 surviving fallback：

```text
root
  └── shared system prefix
        └── typed rewrite
              └── endpoint
```

该关系用于：

* 候选回退；
* 计算 checkpoint被删除后的重建成本；
* 计算 KV page边际价值；
* 按层级降低 continuation，而不是整体删除。

它是稀疏恢复关系，不是完整 radix tree。

---

## 13. Prefix identity

Exact identity由 target Program定义，可能包含：

* token IDs；
* token types；
* position IDs；
* MRoPE axes；
* RoPE delta；
* Vision spans；
* media content digest；
* target template/runtime mode；
* checkpoint frontier。

以下内容只能作为 hint：

* SessionKey；
* request ID；
* raw string；
* caller cache marker；
* hash。

早期设计已经明确缓存命中不能依赖 session ID、原始字符串或未经验证的 hash。

### 13.1 产品输入边界

调用方通过有界的request cache hints表达复用与capture意图：

```text
optional SessionKey
retention hint
bounded stable-prefix markers
allow reuse
```

具体C++字段和容器不是本架构contract，但每个request的markers数量必须由启动配置或产品常量限制，避免一个request产生无界candidate/capture工作。Frontend将marker转换为当前prepared prompt中的合法token frontier；同一request的markers引用一份immutable prompt backing的frontier slices，不按marker复制完整prompt。Program仍对完整target identity执行exact verification。

`SessionKey`只用于lineage ownership、latest replacement和retention。Stable marker只建议capture，retention class只影响策略；三者都不能证明prefix相等，也不能绕过Program verification。Catalog天然绑定当前Engine/model instance，不引入额外runtime target discriminator。

---

## 14. 请求命中与资源准备

### 14.1 Scheduler 先选择请求

Scheduler按现有公平性规则选择一个 waiting request。

ResourceManager不能因为另一个请求的 cache更热而自行换请求。

---

### 14.2 候选枚举

ResourceManager为该请求枚举：

```text
1. Session endpoint
2. Session typed rewrite
3. Explicit shared stable prefix
4. Other matching shared prefixes
5. Optional long anchors
6. Empty/root state
```

前五类候选都由ContentPrefixIndex的内容key产生；顺序只表达ownership/retention偏好，不以SessionKey或
marker身份放宽匹配。每个shortlist hit仍逐个接受Program exact inspection，root始终是确定的最终回退。

---

### 14.3 Program exact inspection

Program对每个候选执行无副作用检查，返回：

```cpp
struct ResumeAssessment {
    bool exact_match;

    CheckpointRef checkpoint;
    uint32_t reused_prompt_tokens;

    TargetKVRequirement required_kv;
    ProgramActivationPlan activation;  // opaque Program capability
    ResourceDemand demand;

    uint32_t remaining_prefill_tokens;
};
```

Program负责理解：

* GDN state payload；
* Main/Backend frontier关系；
* MRoPE/Vision identity；
* typed rewrite semantics；
* target-specific finalization。

ResourceManager不解释模型数学。

---

### 14.4 ResourcePlan

ResourceManager为每个 exact candidate生成：

```cpp
struct ResourcePlan {
    CheckpointRef source;

    LaneId destination_lane;

    ProgramActivationPlan activation;
    ResourceDemand demand;

    ReplicaTransitionIntentSet demotions;
    LogicalEvictionSet evictions;

    PredictedCost cost;

    RevisionSet revisions;
};
```

ResourceManager选择逻辑source、victims和policy intents；`ProgramActivationPlan`及`ResourceDemand`由Program在inspection时生成，并在transaction reserve时复核。ResourcePlan不保存ResourceManager自行选择的raw state slot、physical page ID、replica coverage或physical generation。

`ResourceDemand`表达以下架构语义：

```text
candidate-specific full-lifetime entitlement upper bound
materialization destination/peak reservations
source reuse or Move产生的net delta
typed conditional release credits及其dependencies
```

“精确”表示对所选candidate、request最大有效输出及bounded provisional growth的精确资源上界，不表示预测实际生成长度。ResourceManager验证Program给出的向量，但不根据token数重新推导target page geometry。

#### ProgramActivationPlan

可能为：

```text
FullReset
MoveDevice
ForkDevice
RestorePrivateHost
LoadSharedHost
```

#### Typed KV demand

每个 pool分别描述：

```text
pages already Device-resident
pages requiring H2D
new private pages
shared references
tail copy
future reserved pages
```

#### Demotions

只删除某层 replica，logical checkpoint仍存在：

```text
Device state → Host
Device KV pages → Host
Device duplicate → release
```

#### Evictions

删除 logical checkpoint或降低 continuation：

```text
drop endpoint, preserve rewrite
drop rewrite, preserve endpoint
drop long anchor
drop entire continuation
```

规划阶段没有 physical side effect。任何opaque capability、catalog revision或resource summary在transaction第一次mutation前都必须统一复核。

---

## 15. Candidate 选择

最长 prefix不等于最佳 candidate。

候选总成本为：

\[
T(c) = T_{\text{state materialization}} + T_{\text{KV materialization}}
     + T_{\text{remaining prefill}} + T_{\text{resource transition}} + L_{\text{victims}}
\]

其中：

* state materialization：Move、D2D、H2D 或 Fork；
* KV materialization：所有缺失 pages的 H2D；
* remaining prefill：从 checkpoint到 prompt end的计算；
* resource transition：victim D2H、tail copy、mapping更新；
* victim loss：其他 checkpoints被降级或删除后的未来代价。

不能机械地选择最长 prefix。早期方案也已经明确最长 RAM checkpoint不一定优于较短但完全 Device-resident 的 checkpoint。

---

### 15.1 Victim loss

Checkpoint被删除后的损失近似为：

\[
L(c) = P_{\text{future hit}}(c)
       \cdot \left(T_{\text{rebuild from fallback}} - T_{\text{restore checkpoint}}\right)
\]

再结合：

* live session bonus；
* shared fan-out；
* explicit stable-prefix bonus；
* recency；
* historical hit count；
* 独占资源量。

LRU可以作为同等价值下的 tie-breaker，但不应成为主要策略。

---

### 15.2 资源按增量价值计算

一个 80K endpoint如果共享了 32K system prefix，其独占成本不是 80K，而是：

```text
endpoint state
+ private suffix pages
+ partial private tail
```

逐出时只计算实际能够释放的 Device/Host资源。

---

### 15.3 Scheduler 与 ResourceManager 握手

Scheduler仍然先按既有FIFO、protected-head和backfill规则选择一个有资格尝试admission的request。ResourceManager只为这个request比较cache candidates，不能因为另一个waiting request命中更长prefix而替换Scheduler选择。

Candidate inspection和selection保持只读。ResourceManager向Scheduler暴露candidate-specific的resource demand、reuse/cost、service-work和readiness summary，不暴露source capabilities、physical handles或victim set。Scheduler拒绝choice时没有side effect；接受后由同一个Engine worker立即进入materialization reserve，不能把未pin的choice跨boundary缓存。

Readiness只有四种语义：

```text
Ready
NeedsTransfer
TemporarilyBlocked
PermanentlyInfeasible
```

#### Ready

Selected source与required resources已经Device-ready，且完整active entitlement可以立即reserve。Scheduler接受后在当前worker boundary完成materialization和Active publication。

#### NeedsTransfer

Plan在总容量上可行，但需要D2H/H2D、tail copy或依赖release credit的capacity preparation。Scheduler接受后：

* request进入`MATERIALIZING`，尚不属于active compact batch；
* private source进入`Claimed`或shared source被pin；
* active concurrency reservation \(Q_r\)、完整entitlement和transfer destinations立即进入账本；
* 该request继续保持其protected-head公平性，后续request不能消耗已保留资源或仅凭更热cache越过它；
* 如果transfer异步执行，Scheduler可以继续驱动既有active requests。

Transaction publication后，request从`MATERIALIZING`转为`Active`；publication前的cancellation走`Aborted`路径。

#### TemporarilyBlocked

Request在Engine空闲或当前active resources释放后可以运行，但此boundary无法取得完整reservation。该结果不claim source、不pin victim、不创建transaction，也不会在每个decode boundary轮询。Waiting queue、active entitlement、admission gate或context transaction/capability发生相关变化后，Engine才重新置位admission check；Scheduler随后在下一个符合原有保护、backfill和decode公平性规则的boundary重新执行fresh inspection。

#### PermanentlyInfeasible

所有exact candidates（包括root/full-reset）都无法在配置的per-sequence限制和total capacities内形成完整request-lifetime plan。这是request-specific planning error，不应通过逐出更多cache或反复重试掩盖。

Shared candidate可以通过unique-page sharing使root plan原本需要的重复physical occupancy变为可行，但admission必须把这些shared pages和全部future growth一并pin/reserve；不能依赖尚未进入账本的偶然residency。

---

## 16. Admission transaction

### 16.1 Active 生命周期保证

NInfer不抢占 active request。

Admission时为所选candidate取得完整request-lifetime entitlement：

```text
selected checkpoint required prefix residency
+ remaining prompt growth
+ maximum effective output growth
+ target-defined bounded provisional growth
```

Program基于具体candidate给出精确上界和net delta；ResourceManager不把“剩余token数”直接换算成pages。

部分 KV offload只能发生在 inactive continuation上，不能从 active request借 pages给另一个请求。

---

### 16.2 统一容量账本

Device state遵循第4.1节的 \(C+H\) 方程。每个Device typed KV pool \(s\) 在稳态边界满足：

\[
U_s + E_s + T_s \le K_s
\]

其中：

* \(U_s\)：unique mapped Device page replicas；
* \(E_s\)：active reserved-but-unmapped growth entitlement；
* \(T_s\)：transaction、tail-copy和COW destination reservations；
* \(K_s\)：该typed pool的physical capacity。

同一个shared Device page无论被多少address spaces或active requests引用，都只进入 \(U_s\) 一次；新的logical reference本身不增加physical occupancy。

Host KV满足：

\[
B_{\text{published Host extents}} + B_{\text{reserved Host targets}}
\le B_{\text{HostKVArena}}
\]

Transaction可以使用有依赖条件的release credits。对任一resource dimension \(r\)，reservation可行性为：

\[
\operatorname{used}_r - \operatorname{eligible\_release}_r + \operatorname{destination}_r
\quad + \operatorname{other\_reservations}_r \le \operatorname{capacity}_r
\]

Release credit只有在以下条件同时成立时才可进入方程：

* 它对应一个确定且没有active/source/shared pin的unique physical resource；
* 若logical object仍需保留，替代Host或Device replica已经完整发布；
* 若credit来自logical eviction，该eviction已经成为transaction的不可逆capacity-preparation结果；
* 对应typed pool、bytes/slots数量和dependency由Program精确确认。

Release credit只是reservation依据。物理执行的每一步仍不得瞬时超过实际pool capacity；依赖该credit的destination只能在相应replica真正释放后分配。ResourceManager维护policy reservation ledger，Program返回并核对exact physical deltas，二者都不能根据shared reference数量重复计费。

---

### 16.3 MaterializationTransaction

Materialization不是固定十五步列表，而是一个带依赖关系的transaction：

```text
Inspected
    → Reserved
    → Transferring
    → ReadyToPublish
    → Published
    → Finalized

Reserved / Transferring / ReadyToPublish
    → Aborted
```

State、Main KV、Backend KV、victim demotion、logical eviction和tail copy可以形成不同DAG，但都遵循同一个publication point。

#### Reserved

唯一Engine worker在第一次mutation前完成：

1. 复核request、catalog revisions与全部Program capabilities；
2. claim并pin private source，或pin shared source；
3. pin selected victims及其release dependencies；
4. 原子验证完整entitlement、transaction peak和conditional release credits；
5. reserve Host/Device destinations及ResourceManager的adoption bookkeeping；
6. 创建唯一的Program transaction ticket。

Private source此时进入 `Claimed`，从普通candidate discovery中隐藏；它不再称为可发现的 `Catalogued` entry。Request尚未成为Active，也没有用户可见publication。

#### Transferring

Program按依赖图执行source-preserving transfers。例如Device KV已满且destination依赖victim release时：

```text
reserve Host target
→ victim D2H
→ publish transaction-owned Host replica
→ release victim Device replica
→ allocate destination Device page
→ source H2D
```

Victim Device replica只有在替代replica完整发布后才可作为demotion credit释放。若capacity preparation选择直接删除低价值logical victim，则该eviction可以在Active publish之前提交，以兑现release credit。

因此transaction abort不承诺恢复所有cache placement或被选victims；合法demotion/eviction可以保留。Abort保证的是selected source仍有效、不会出现半发布ActiveSequence，并且Program与ResourceManager账本保持一致。

Transfer可以同步完成，也可以通过独立stream和completion event跨worker boundaries推进，两种实现具有相同publication contract：

* in-flight source、destination和对应release dependency始终被pin；
* copy完成前不改变published residency或兑现release credit；
* completion只由唯一Engine worker采用，并再次核对transaction ticket、capability generation、content epoch和coverage；
* cancellation或尚未发生CUDA integrity failure的合法abort保留source并返回可核对的`Aborted` physical delta；CUDA copy失败或completion状态不可信时进入Engine-wide failure；
* 异步transfer期间request保持`MATERIALIZING`，现有active decode可以继续，但不得使用该transaction已reserve的resources。

Host表示仍是packed logical order；实际D2H/H2D按连续physical runs合并，避免逐page提交大量copy。是否使用独立stream及具体overlap策略属于Program实现，不改变ResourceManager/Scheduler语义。

#### ReadyToPublish

进入该状态时：

* active state binding可执行且所有transfer完成：非Fork destination state完整，Fork source完整并保持pin、destination slot已独占预留；
* required KV pages已Device-ready；
* block-table内容已准备；
* 完整active entitlement已保留；
* source capability仍然有效并被pin；
* publish后的logical adoption不再需要分配内存。

#### Published

Program transaction commit是唯一物理publication point。它安装block tables和active physical binding，消费transaction ticket，并返回`SequenceHandle`及exact resource delta。随后同一个worker boundary内以预留、不可失败的bookkeeping完成：

```text
Program physical publish
→ RequestRecord adopts SequenceHandle
→ ResourceManager ledger adopts exact delta
→ private Claim转为Active ownership / shared refs增加
```

对worker之外，这些步骤只呈现为一次Active transition。Program commit之前的失败走transaction abort；commit之后如果capability或resource acknowledgement不一致，Engine无法安全replan，必须进入Engine-wide cleanup/failure。

#### Finalized

Finalize释放source/victim pins、未使用reservations和transaction ticket，并执行尚未为capacity preparation提前完成的policy cleanup。已经为release credits完成的demotion/eviction不需要等待Finalize，也不会因request随后取消而自动回滚。

#### Aborted

Publish前abort释放destination和未使用reservations；private source从`Claimed`恢复为`Catalogued`，shared source只解除pin。不得发布ActiveSequence，也不得留下未核对的physical delta。

核心不变量是：

> Destination 在完整发布前，source 始终保持有效。

Private Host source只能在 ActiveSequence成功发布后被 consume。

MaterializationTransaction不consume shared source；transaction解除pin后，它仍可由后续独立retention policy合法降级或逐出。

---

### 16.4 Source protection

一个已被选为resume source的checkpoint，在transaction完成前：

* 不能被policy或其他plan逐出；
* 不能被拿去为另一个transfer腾出空间；
* 不能改变identity revision或content epoch；
* 不能删除其最后一份有效state/KV replica。

选中的victim在其dependency完成前同样被pin。Release完成后，该victim的placement或logical lifetime由已提交的capacity-preparation结果决定，不再要求恢复到transaction之前的状态。

---

## 17. Checkpoint 创建

### 17.1 只从 committed state发布

Checkpoint只能在 Program已经提交：

* state；
* Main KV frontier；
* Backend KV frontier（如果存在）；
* token ledger；
* target identity；

之后发布。

Rejected speculative suffix、尚未提交的replica coverage和`fork_pending` destination不能进入catalog。Program commit显式报告完整target state是否已经形成；只有state image与全部typed KV requirement一致时才能发布checkpoint。

---

### 17.2 Typed rewrite capture

当 prefill到达 typed rewrite frontier：

```text
prefill to frontier
→ commit
→ publish rewrite checkpoint
→ continue suffix
```

优先使用：

```text
Freeze current state
→ Fork next active state
```

Private continuation的 KV address space继续线性增长，无需为同一 private branch复制 partial tail page。

如果没有额外 Device state slot：

* 可以先将 checkpoint state snapshot 到 Host，再继续原地执行；
* 可以下沉其他低价值 Device checkpoint；
* 若 capture是可选的，可以放弃 capture。

---

### 17.3 Shared stable prefix capture

到达 stable frontier后：

```text
commit stable frontier
→ Freeze shared state
→ publish immutable full KV pages
→ copy partial tail for current active branch
→ Fork state到新的 active slot
→ continue private suffix
```

高价值shared checkpoint的默认retention policy倾向于建立Host state backup；这不是checkpoint有效性的正确性条件。当Host state capacity为零或不足时，Device-only shared checkpoint仍然有效，只是在最后一份Device replica被删除时随之失效。

---

### 17.4 Endpoint publication

Request完成后：

```text
ACTIVE state slot
→ private endpoint checkpoint
```

这是 ownership reclassification，不需要 state copy。

如果request从shared prefix或root启动，而同一SessionKey仍有旧catalogued endpoint，则先完整发布新endpoint并原子更新SessionIndex，再释放旧endpoint，避免中间失败导致session失去全部恢复点。

如果旧private endpoint已经通过destructive Move进入active ownership，它在publish transaction时已经离开catalog，不存在一个仍可独立替换的旧endpoint。正常finish只需将当前完整active state Freeze为新endpoint并返回catalog。

---

### 17.5 Checkpoint replacement

默认每个 private session保留：

```text
latest endpoint
latest typed rewrite
```

对于仍然独立存在于catalog的旧checkpoint，新的checkpoint必须先完整发布，再删除旧checkpoint。已经被claim并转入active ownership的private source不适用这一替换顺序。

删除旧 endpoint时，如果 rewrite仍然存在：

* continuation可以缩短到 rewrite frontier；
* rewrite所需 KV prefix继续保留；
* endpoint独占 suffix可以释放。

---

### 17.6 Cancellation 与失败

Publish前的request cancellation中止MaterializationTransaction，并将private source从`Claimed`恢复为`Catalogued`。Publish后的cancellation默认释放active resources且不创建新checkpoint；destructive Move之前的旧endpoint不保证恢复。

Program execution或commit失败遵循Engine-wide failure contract，不尝试从可能不一致的active physical state捕获checkpoint。

---

## 18. Resource pressure

State slot和 KV page是独立资源，必须独立决策。

### 18.1 Device state pressure

优先动作：

1. 删除已有 Host replica的低价值 Device state replica；
2. 将 cold Device checkpoint state下沉 Host；
3. 删除被新 checkpoint取代的旧 state；
4. 删除低价值 rewrite或long anchor；
5. 降级或删除整个 continuation；
6. 如果只是 optional capture，跳过 capture。

Active state永不逐出。

---

### 18.2 Device KV pressure

优先动作：

1. 释放 refcount 为零的 pages；
2. 删除已有 Host replica的低价值 Device replicas；
3. 将 private endpoint在 rewrite之后的 suffix逐出 Host；
4. 将 inactive continuation的冷 pages逐出；
5. 尽量保留 shared stable prefix；
6. 必要时删除 endpoint并将 continuation缩短到 rewrite；
7. 最后才删除完整高价值 continuation。

KV逐出以满足实际 deficit 为目标，不应无条件整体 park。

---

### 18.3 Host state pressure

优先删除：

1. 已被替代的 checkpoint；
2. Disposable request的 checkpoint；
3. low-value long anchor；
4. 低命中 typed rewrite；
5. 低 fan-out shared state；
6. 整个低价值 private continuation。

删除 rewrite只释放一个 state image，endpoint仍可保留。

删除 endpoint可能使 continuation退化为 rewrite-only。

---

### 18.4 Host KV pressure

优先删除：

1. 无 logical reference的 Host extents；
2. Device已有完整 replica的低价值 Host duplicate；
3. Disposable continuation的 private suffix；
4. 已删除 endpoint之后不再需要的 suffix；
5. 距离 surviving fallback很近的 checkpoint pages；
6. 低 fan-out shared pages。

如果删除某 Host extent会导致某 checkpoint所需 page无任何 replica，则必须：

* 同时删除该 checkpoint；
* 或保留另一份 replica。

---

## 19. 典型场景

### 19.1 Agent endpoint append

输入：

```text
latest endpoint + new tool result / user input
```

流程：

```text
找到 private endpoint
→ exact verification
→ Move Device state或Restore Host state
→ materialize缺失 KV pages
→ claim private KV address space
→ reserve future pages
→ prefill new suffix
```

---

### 19.2 Endpoint mismatch，Rewrite match

原因可能包括：

* tool call JSON重新序列化；
* 模型输出格式化变化；
* 空白符变化；
* caller wrapper变化。

流程：

```text
endpoint verification fails
→ typed rewrite succeeds
→ restore rewrite state
→ drop endpoint state
→ trim logical KV frontier to rewrite
→ release endpoint-only suffix pages/extents
→ prefill divergence suffix
```

---

### 19.3 并发为 1 的辅助请求

长 continuation占用 230K KV，新请求需要 32K，但当前只剩 26K。

ResourceManager只需：

```text
从 inactive长 continuation逐出约6K page-rounded suffix KV到Host
```

而不是整体逐出230K。

State方面：

* 若有空闲 Device state slot，长 continuation state继续热驻留；
* 否则只把 state下沉 Host；
* 短标题请求通常完成后直接释放，不保留其低价值 endpoint。

---

### 19.4 长 Host checkpoint与短 Device checkpoint

候选：

```text
128K Host-mixed checkpoint
96K Device-ready checkpoint
```

ResourceManager比较：

```text
恢复缺失 state/KV + prefill 2K
```

与：

```text
零恢复 + prefill 34K
```

选择预测总成本更低的方案，而不是机械选择128K。

---

### 19.5 Shared system prompt

一个 32K shared prefix：

```text
state：immutable checkpoint
KV：immutable shared prefix pages
```

每个请求：

```text
Fork state
→ retain full shared pages
→ copy partial tail if needed
→ allocate private suffix
```

多个 active requests共享相同 full pages。

---

### 19.6 只有 KV，没有 state

即使某段 KV仍存在：

```text
没有对应 state checkpoint
→ 不能直接恢复
```

ResourceManager回退到更早 checkpoint或 full reset。

无 checkpoint引用的 KV pages可以回收。

---

## 20. Retention policy

Checkpoint按逻辑价值分为：

### Pinned

* Active resources；
* 正在迁移的 source/destination；
* 已被 admission plan选中的 checkpoints/pages；
* 当前 shared active refs。

不可逐出。

### Shared Stable

* 高 fan-out system prompt；
* 显式 caller stable prefix；
* 多 session共同工具定义。

### Live Session

* 活跃 Agent最新 endpoint；
* 活跃 Agent最新 rewrite；
* 长上下文 private continuation。

### Recent Private

* 最近命中的普通 endpoint；
* 有较高重建成本但无显式 session hint的 continuation。

### Disposable

* 标题/摘要等短辅助请求；
* 已被替代的旧 checkpoint；
* 短且重建便宜的匿名 continuation；
* 长期无命中的 cold entry。

实际 victim选择仍使用边际资源价值，不只依赖类别。

---

## 21. Program 与 ResourceManager 的 Contract

### 21.1 ResourceManager 拥有

* ContinuationCatalog；
* session/shared indexes；
* continuation/checkpoint逻辑生命周期与policy visibility；
* state/KV资源价值；
* candidate selection；
* demotion/eviction计划；
* active admission与transaction reservation ledger；
* Program铸造的opaque capabilities及revisioned summaries的custody。

ResourceManager不拥有实际Device/Host residency、replica coverage或physical generation。它依据Program返回的summary和delta做策略与账本判断，不能维护第二份可独立修改的physical事实。

### 21.2 Program 拥有

* target exact identity；
* StateImageStore及完整state physical layout；
* KVAddressSpace objects及其ordered logical-page membership；
* LogicalKVPageStore；
* DeviceStatePool与HostStatePool；
* Device KV allocators与Host KV extent allocator；
* physical slot/page/capability generations；
* replica content epoch、coverage和transfer state；
* Device/Host state copy；
* KV plane layout；
* page materialization；
* block-table安装；
* GDN/Backend frontier语义；
* state src/dst selector；
* ActiveStateBinding和Fork resolution；
* prefill/decode/commit；
* checkpoint capture的数学正确性。

Program是physical residency和resource acknowledgement的唯一authority。ResourceManager选择“哪个逻辑对象值得恢复或牺牲”，Program决定并验证“哪些physical replicas、slots和bytes实际存在并如何转换”。

### 21.3 核心接口

概念上：

```cpp
RequestBasePlan describe_request(...);

ResumeAssessment inspect_checkpoint(
    const PreparedRequest& request,
    const CheckpointRef& checkpoint,
    LaneId destination);

MaterializationOutcome materialize(
    ResourcePlan&& plan,
    CancellationView cancellation);

CheckpointCapture capture_checkpoint(
    SequenceHandle sequence,
    CheckpointSpec spec);

EndpointPublication finish_sequence(
    SequenceHandle&& sequence);

ReplicaTransitionResult transition_replicas(
    ReplicaTransitionPlan&& plan);
```

Inspection无副作用。

`materialize`在Program内部独占physical transaction ticket，并线性消费ResourcePlan。它只返回`Published`或已经完成physical cleanup/accounting acknowledgement的`Aborted` outcome，不把unresolved ticket或半所有权source返回给caller。两类outcome都携带需要由worker原子采用的exact deltas；`Aborted`也包含已经合法持久化的victim demotion/eviction结果，ResourceManager先采用这些delta，再把private source从`Claimed`恢复为`Catalogued`。

接口名和C++聚合形式只是概念示意；跨模块必须保留opaque activation/ticket/sequence/state/page capabilities、exact demand/delta和consumption acknowledgement的语义。ResourceManager不能通过这些接口获得可自行修改的raw physical IDs。

所有mutation只能由唯一Engine worker在execution boundary调用。异步transfer可以跨CUDA event完成，但其publication和replica metadata mutation仍回到该worker，并复核ticket、capability generation及content epoch。

### 21.4 失败分类

以下情况发生在第一次Program mutation前，且没有active/source physical state被改变，可以丢弃choice并重新inspection、选择更短checkpoint或full reset：

* exact identity mismatch；
* catalog choice/revision已经合法失效；
* candidate replica已经被合法policy transition删除；
* reservation前发现Device/Host容量不足；
* optional checkpoint capture无法取得容量；
* 最新residency下candidate不再具有成本优势。

以下情况说明Program physical事实与ResourceManager账本可能不再一致，不能伪装成cache miss：

* Program capability owner/generation mismatch；
* resource consumption/release acknowledgement mismatch；
* logical reference或unique occupancy accounting violation；
* replica content epoch/coverage invariant被破坏；
* CUDA copy、launch或transfer completion状态不可信；
* Program commit/rollback invariant failure；
* physical publication之后发生无法闭合的异常。

这些情况进入Engine-wide cleanup/failure。所谓“cache不改变模型正确性”是指合法miss、replan和eviction只影响性能；它不意味着内部完整性错误可以降级成full prefill。

---

## 22. 正确性不变量

1. 一个active request独占一个lane；
2. Program是state/KV physical residency、generation和coverage的唯一authority；
3. 一个mutable Device state slot同一时间只能属于一个active sequence；
4. immutable checkpoint state永远不可原地修改；
5. 多个rows可以共享immutable source slot，但destination slots必须不同；
6. Fork只有在Program commit确认完整target state后才能解除source pin；
7. 每个checkpoint state精确对应其token frontier；
8. 每个checkpoint required KV page至少存在一份content epoch一致、coverage充足的replica；
9. 只有全部required state/KV位于Device且future entitlement已取得时，checkpoint才是Device-ready；
10. 仅有KV不能构成prefix hit；
11. SessionKey、hash、retention和caller marker不能替代target exact verification；
12. 一个private continuation同一时间最多处于一个`Claimed`或`Active` ownership；
13. shared prefix只能Fork，不能被destructive Move；
14. private continuation内部checkpoint与active可以共享single-writer append-only tail；
15. private writer不得覆盖任何surviving checkpoint保护的prefix；
16. shared多分支写入partial tail前必须建立private tail/page；
17. active resources永不逐出，完整request-lifetime entitlement在active lifetime内不可撤销；
18. Main与Backend KV保持独立typed capacity与reservation ledger；
19. 所有checkpoint只从Program committed state及committed KV coverage发布；
20. rejected speculative state/KV不能进入catalog或canonical replica coverage；
21. shared physical state/page无论被多少逻辑对象引用，容量只计一次；
22. 若logical object仍需保留，physical replica释放前必须存在满足其requirement的替代replica；否则必须显式提交logical eviction；
23. Release credit只有在其dependency完成且unique physical resource可实际释放后才能兑现；
24. 已选resume source在transaction publication或abort前始终有效并被pin；
25. Publish前abort不得留下半发布ActiveSequence，但不要求回滚已合法提交的victim demotion/eviction；
26. stale catalog choice可以在mutation前replan，Program capability或physical acknowledgement mismatch必须进入Engine-wide failure；
27. Engine/model instance销毁时所有continuations和Program capabilities自动失效。

---

## 23. 性能原则

### 23.1 普通 decode不受影响

普通 decode保持：

```text
state_src == state_dst
```

slot selector在kernel inner loop之外解析。普通row在Fork完成后立即回到`state_src == state_dst`，不会为context cache长期承担额外state copy。

Fork的`src != dst`只持续到首个使destination成为完整committed target state的Program commit；它可能跨过不形成完整state的exact-hit finalization，但不由Engine热路径推断。

---

### 23.2 避免完整 state copy

优先：

```text
Freeze + Fork
```

而不是：

```text
copy current → checkpoint
```

Private checkpoint恢复优先使用 Move。

---

### 23.3 只迁移真正缺失的 KV

Host restore按 checkpoint required prefix和当前 Device residency计算。

只恢复：

```text
missing logical pages
```

而不是整个 parked continuation。

---

### 23.4 只逐出实际缺口

Device KV缺6K，只逐出足够覆盖 page-rounded 6K 的低价值 extents。

不整体迁移230K continuation。

---

### 23.5 批量 transfer

Host KV extent内部按 logical order打包，但 transfer应合并连续 physical page runs：

```text
logical canonical storage
+ physical coalesced transfer
```

减少 memcpy提交数量和 PCIe事务开销。

---

### 23.6 实测成本模型

ResourceManager不从请求流在线学习成本。Engine启动时只读解析一个不可变`ContextCostModel`，所有请求
共享同一组系数，请求执行不会修改它。简化模型为：

```text
transfer = max(batch_ns + copy_operations * operation_ns,
               payload_bytes * ns_per_byte)
prefill = chunks * chunk_ns
        + suffix_tokens * token_ns
        + attention_pairs * attention_pair_ns
        + vision_items * vision_item_ns
        + vision_patches * vision_patch_ns
```

transfer只按D2H、H2D、D2D方向各持有一组本机系数。`payload_bytes`是CUDA实际搬运的payload，排除
Host arena的对齐padding；`copy_operations`是该操作实际提交的`cudaMemcpy*`次数。PageMajor Host
restore按`planes * contiguous_runs`计数，HeadMajor按`sum(heads) * contiguous_runs`计数，Device页拷贝
按`planes * pages`计数，StateImage按实际component copy计数。因此Main-KV、Backend-KV、State、KV
dtype和speculative backend都不再成为成本preset维度：它们造成的物理差异已经落在bytes和operation
数量中。resource class只保留为资源会计与观测分类。D2H计划在源页已分配时使用实际physical runs；
H2D候选在目标页尚未分配时以已知Host extent runs作名义值，完成后的observation再按Host extent与实际
目标physical runs的共同切分记录真实调用数，不为预测额外预分配或重选目标页。

其中从prefix frontier `B`重建suffix `S`时：

```text
attention_pairs = B*S + S*(S+1)/2
```

无额外边界时`chunks = ceil(S / prefill_chunk)`；请求计划已知capture/rewrite边界时，按实际segment分别
向上取整后求和。Program从checkpoint/prompt frontiers直接生成这些feature。尤其checkpoint降级到更早
fallback时重新按`B,S`计算，不能相减两个累计chunk数；无法从历史checkpoint摘要精确恢复segment边界的
区间继续使用单区间近似，而不为此保存完整schedule元数据。

数值模型始终存在。解析顺序是generic numerical default、匹配的compiled default、匹配的external
preset；transfer与prefill独立分层，任一层缺失都保留上一层数值。transfer lookup只使用hardware
class；prefill lookup使用hardware class和artifact `model_id/weights_id`。有效外部文件没有匹配项不是
错误；文件格式或数值无效才在启动时失败。ResourceManager始终先比较预测nanoseconds，仅在数值完全
相同时使用确定性的语义tuple打破平局。

编译期默认值的唯一authority为`src/runtime/engine/context_cost_defaults.cpp`。JSON不参与CMake或构建；
它只是运行时本机预设。schema以hardware为顶层记录，每个记录可独立包含一组transfer与多个artifact
prefill项：

```json
{
  "schema_version": 2,
  "artifact_type": "ninfer_context_cost_presets",
  "machines": [{
    "hardware_class": "nvidia-geforce-rtx-5090-sm120",
    "transfer": {
      "d2h": {"batch_ns": 48655, "operation_ns": 7433, "ns_per_byte_q32": 91692315},
      "h2d": {"batch_ns": 0, "operation_ns": 8457, "ns_per_byte_q32": 83354284},
      "d2d": {"batch_ns": 3343, "operation_ns": 9520, "ns_per_byte_q32": 2658314}
    },
    "prefill": [{
      "model_id": "qwen3.6-27b",
      "weights_id": "groupwise-int",
      "coefficients": {
        "chunk_ns": 40813570,
        "token_ns_q32": 1012273154411951,
        "attention_pair_ns_q32": 30497396515,
        "vision_item_ns": 5986585,
        "vision_patch_ns_q32": 23710212854694
      }
    }]
  }]
}
```

离线calibrator的transfer suite使用合成PageMajor/HeadMajor fixture和生产copy primitives，并补充bytes与
operation数量独立变化的连续D2D batch，以覆盖StateImage component并辨识device bandwidth；它不读取
artifact也不加载模型。prefill suite才通过公共Engine加载一个artifact。两套suite可分别原子更新同一
preset，不会覆盖另一组件或其他artifact项。

运行时仍持续记录实际行为用于观测和下一轮离线校准：

* state D2H/H2D latency；
* checkpoint hit类型；
* endpoint mismatch比例；
* shared prefix fan-out；
* 实际 saved prefill tokens；
* demoted checkpoint后续命中率。

其中State/KV transfer observation只更新`RuntimeStats`的count/bytes/seconds，不回写成本系数。Text与
Vision prefill系数由独立calibrator在真实artifact上测量；详细report与可加载preset分离，只有通过
training/held-out error与ordering验收的结果才能原子写入preset。

校准验收把materially different点的严格预测逆序视为失败；简化feature/roofline无法区分而产生的精确
预测平局单独报告，但不拒绝，因为运行时本来就会把精确nanoseconds平局交给确定性的semantic tie-break。

---

## 24. 配置与容量

当前启动配置为：

```cpp
struct ContextCacheOptions {
    bool enabled = true;

    optional<uint32_t> device_state_slots;  // extra Device checkpoint capacity H
    uint32_t host_state_slots = 8;           // R
    size_t host_kv_capacity_bytes = 8ULL << 30;

    optional<uint32_t> max_private_continuations;
    optional<uint32_t> max_shared_prefixes;
    optional<uint32_t> max_long_anchors_per_continuation;
    optional<uint32_t> max_cache_markers_per_request;
};

struct ContextCostOptions {
    filesystem::path preset_path; // empty: generic + matching compiled defaults
};
```

`Engine`在构造target前只解析一次optional。设`C=max_concurrency`，启用时的有效默认值为：

```text
H = C
R = 8
Host KV bytes = 8 GiB
P = 2C private continuations
S = C shared prefixes
L = 2 long anchors per private continuation
M = 4 caller markers per request
```

`Engine::options()`返回全部optional已经具体化的有效配置。`enabled=false`解析为root-only：`H/R/Host
KV/S/L=0`、`P=C`，成功完成的请求不发布continuation；`M`仍约束输入复杂度。

Device state总量：

\[
C + H
\]

默认数值上是 `2C` Device state images，但语义是：

```text
C active guarantee
+ C global Device-checkpoint capacity
```

而不是每 lane固定 current/rewrite配对。

State capacity必须在最终 KV capacity求解前进入 Device memory planning。

Host state按 image数量计费。

Host KV按实际 extent bytes计费。

---

## 25. Observability

`RuntimeStats`在worker boundary发布三类数据：Engine构造以来的累计counter、当前resource/scheduler
gauge，以及显式命名的最后一次决策观测。当前字段覆盖：

* materializing/capture-pending request gauge与ActiveCapture完成/中止counter；
* 六类selection counter、累计reused prompt tokens和`last_selected_frontier_tokens`；
* state Move/Fork/Restore与D2H/H2D/D2D count、bytes、seconds；
* Main/Backend KV D2H/H2D/D2D pages、bytes、seconds；
* partial spill、partial-tail COW、private/shared degradation和eviction counter；
* `device_state_occupied_slots`、`host_state_occupied_slots`、Device Main/Backend pages、Host KV
  bytes与shared active-reference gauge；
* historical Fork hit counter；
* `last_predicted_materialization_ns`；
* 累计actual context-transfer seconds。

`LoadSummary.context_cost`分别报告transfer/prefill来源（generic-default/compiled-default/external）、
hardware class、artifact `model_id/weights_id`及外部preset路径；serve启动console与JSONL server-start
record都发布该信息。

`MemorySummary`另行报告启动固定的Host State/KV capacity和调用边界上的physical occupancy。serve
JSONL把累计counter转换为interval delta，保留当前gauge和last-decision语义；不把进程级delta归因到
单个request。

这些数据是后续调优 (H)、Host容量和 retention policy 的依据。

---

## 26. 架构总结

最终架构由四个核心概念组成：

```text
Continuation
    表示一条逻辑序列历史和其 KV address spaces

Checkpoint
    表示该历史中某个精确 frontier 的完整 state

StateImage
    完整、不可拆分、对应精确 frontier

KVAddressSpace
    可分页、可共享、可部分驻留于 Device/Host
```

ContinuationCatalog保存逻辑对象及Program铸造的opaque capabilities；StateImage、KVAddressSpace、LogicalKVPage及其replicas的实际storage事实只存在于Program。

核心资源模型为：

```text
DeviceStatePool = C + H
HostStatePool   = R

Device Main KV Pool
Device Backend KV Pool
Host KV Extent Store
```

核心 state 操作为：

```text
Private resume：Move
Shared resume：Fork
Checkpoint creation：Freeze + Fork
Cold state：Snapshot / Restore
```

核心 KV 机制为：

```text
logical page address space
Program-owned logical page identity
capability generation
committed content epoch / coverage
per-page Device/Host replicas
packed logical-order Host extents
partial residency
exact-deficit spill
shared full pages
shared partial-tail copy
```

核心materialization lifecycle为：

```text
Inspected → Reserved → Transferring → ReadyToPublish
          → Published → Finalized

publish前可Aborted；source保持有效，
已合法提交的victim demotion/eviction不要求回滚。
```

核心调度关系为：

```text
Scheduler
    选择当前请求

ResourceManager
    选择 checkpoint、logical intents 和 victims

Program
    精确验证，拥有physical storage事实，
    执行state/KV transition与model commit
```

该设计能够统一覆盖：

* Agent endpoint append；
* typed rewrite fallback；
* 长session被短请求打断；
* 多个private sessions切换；
* shared system prompt；
* State与KV独立分层；
* KV的部分Host residency；
* 有限VRAM下的精确资源释放；
* 成本感知而非最长前缀优先的恢复策略。

最关键的设计结论是：

> **State 决定“在哪里可以恢复”，KV 决定“恢复该位置需要哪些分页前缀”；State 以完整 image 调度，KV 以 logical page/extent 调度。Continuation 将二者组织成同一条序列历史，但不强迫二者同层驻留或整体迁移。**
