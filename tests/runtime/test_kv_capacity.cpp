#include "runtime/engine/kv_capacity.h"
#include "runtime/engine/startup_memory.h"

#include <iostream>
#include <stdexcept>

namespace {

int check(bool condition, const char* message) {
    if (condition) { return 0; }
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    int failures = 0;
    const ninfer::runtime::SequenceCapacityCurve curve{
        .primary_page_tokens                     = 64,
        .minimum_primary_page_groups             = 2,
        .maximum_primary_page_groups             = 6,
        .minimum_device_reservation_bytes        = 1000,
        .bytes_per_additional_primary_page_group = 128,
    };

    const auto automatic =
        ninfer::runtime::resolve_kv_capacity(ninfer::KvCapacityPolicy::automatic(50), curve, 1360);
    failures +=
        check(automatic.primary_page_groups == 4 && automatic.resolved_tokens == 256 &&
                  automatic.runtime_reservation_bytes == 1256 &&
                  automatic.automatic_headroom_bytes == 50 && automatic.planned_slack_bytes == 104,
              "automatic KV capacity did not select the largest fitting page count");

    const auto capped =
        ninfer::runtime::resolve_kv_capacity(ninfer::KvCapacityPolicy::automatic(50), curve, 10000);
    failures += check(capped.primary_page_groups == 6 && capped.resolved_tokens == 384,
                      "automatic KV capacity exceeded or missed the target maximum");

    const auto explicit_capacity = ninfer::runtime::resolve_kv_capacity(
        ninfer::KvCapacityPolicy::explicit_capacity(129), curve, 1200);
    failures += check(explicit_capacity.primary_page_groups == 3 &&
                          explicit_capacity.resolved_tokens == 192 &&
                          explicit_capacity.runtime_reservation_bytes == 1128,
                      "explicit KV capacity did not use page-aligned token semantics");

    bool insufficient_rejected = false;
    try {
        (void)ninfer::runtime::resolve_kv_capacity(ninfer::KvCapacityPolicy::automatic(50), curve,
                                                   1049);
    } catch (const std::invalid_argument&) { insufficient_rejected = true; }
    failures += check(insufficient_rejected,
                      "automatic KV capacity accepted less than the minimum reservation");

    // The probe left 200 bytes in the driver; the new free snapshot already includes it.
    const ninfer::runtime::StartupMemoryBudget budget({10000, 12000}, 8000, 500);
    failures += check(budget.available({7800, 12000}) == 5300,
                      "startup retained allocation was not charged exactly once");
    failures += check(budget.available({9800, 12000}) == 7300,
                      "destroyed candidate backing was not returned to the budget");
    failures += check(budget.retained({7800, 12000}, 2000) == 200,
                      "owned weights were included in opaque retained storage");
    failures += check(ninfer::device_memory_growth({8000, 12000}, {8100, 12000}) == -100,
                      "negative memory growth wrapped to a large unsigned estimate");
    failures += check(budget.fits({2500, 12000}) && !budget.fits({1900, 12000}),
                      "actual startup footprint did not obey the Device budget");
    failures += check(!budget.fits({2499, 12000}),
                      "final startup accepted a one-byte configured headroom deficit");
    const ninfer::runtime::StartupMemoryBudget physical({3000, 12000}, 8000, 500);
    failures += check(physical.available({2000, 12000}) == 1500,
                      "physical availability did not bound the configured budget");
    failures += check(physical.fits({500, 12000}) && !physical.fits({499, 12000}),
                      "final startup accepted a physical headroom deficit");

    // Native DFlash2 has one shared KV pool, not max_context per active lane.
    // Format is an input byte stride: INT8 and rk4v4, including their scale planes.
    constexpr std::size_t mib = 1ULL << 20;
    constexpr std::size_t state_image = 195897344;
    for (const std::size_t bytes_per_token : {33792ULL, 17920ULL}) {
        for (const std::size_t headroom : {768 * mib, 640 * mib}) {
            for (const std::uint32_t concurrency : {1U, 2U}) {
                const std::uint32_t context = bytes_per_token == 33792 ? 131072 : 262144;
                const std::uint32_t minimum_pages = context / 64;
                // The caller supplies its *complete* measured/planned non-KV
                // reservation, including per-request scratch and graph backing.
                const std::size_t non_kv = (concurrency + 2) * state_image + 256 * mib;
                const ninfer::runtime::SequenceCapacityCurve native{
                    .primary_page_tokens = 64,
                    .minimum_primary_page_groups = minimum_pages,
                    .maximum_primary_page_groups = concurrency * minimum_pages,
                    .minimum_device_reservation_bytes = non_kv + context * bytes_per_token,
                    .bytes_per_additional_primary_page_group = 64 * bytes_per_token,
                };
                const auto available = native.minimum_device_reservation_bytes + headroom;
                const auto fit = ninfer::runtime::resolve_kv_capacity(
                    ninfer::KvCapacityPolicy::automatic(headroom), native, available);
                failures += check(fit.resolved_tokens == context &&
                                      fit.planned_slack_bytes == headroom,
                                  "native shared pool multiplied capacity or spent its reserve");
                bool refused = false;
                try {
                    (void)ninfer::runtime::resolve_kv_capacity(
                        ninfer::KvCapacityPolicy::automatic(headroom), native, available - 1);
                } catch (const std::invalid_argument&) { refused = true; }
                failures += check(refused, "native fit accepted a one-byte reserve deficit");
                if (concurrency == 2) {
                    const auto expanded = ninfer::runtime::resolve_kv_capacity(
                        ninfer::KvCapacityPolicy::automatic(headroom), native,
                        available + 3 * native.bytes_per_additional_primary_page_group - 1);
                    failures += check(expanded.resolved_tokens == context + 2 * 64 &&
                                          expanded.planned_slack_bytes >= headroom,
                                      "native C2 auto pool did not use the actual KV byte stride");
                }
            }
        }
    }

    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
