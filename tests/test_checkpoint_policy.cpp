// Which turns are worth a checkpoint, which refused saves an automatic checkpoint retries, how many
// times, and how a graceful stop accounts a session whose engine continuation is gone.
#include "serve/checkpoint_policy.h"

#include <iostream>
#include <string>

namespace {

using ninfer::runtime::SessionCheckpointSkipDetail;
using ninfer::runtime::SessionCheckpointSkipReason;
using ninfer::runtime::DurableReason;
using ninfer::serve::ShutdownCheckpointOutcome;

int check(bool condition, const char* message) {
    if (condition) { return 0; }
    std::cerr << message << '\n';
    return 1;
}

SessionCheckpointSkipDetail outcome(DurableReason reason) {
    SessionCheckpointSkipDetail skip;
    skip.durable_reason = reason;
    return skip;
}

} // namespace

int main() {
    using ninfer::serve::checkpoint_refusal_is_transient;
    using ninfer::serve::classify_shutdown_checkpoint;
    int failures = 0;

    // The two refusals EXP-049 observed while the sessions were still resident: early, not final.
    failures += check(checkpoint_refusal_is_transient(outcome(DurableReason::TagMismatch)),
                      "a save that ran before the new continuation was catalogued is not retried");
    failures += check(checkpoint_refusal_is_transient(
                          outcome(DurableReason::TransactionBusy)),
                      "a save that ran during another request's transaction is not retried");
    failures += check(checkpoint_refusal_is_transient(
                          outcome(DurableReason::StalePublication)),
                      "a changed publication epoch is not retried after progress");
    failures += check(checkpoint_refusal_is_transient(outcome(DurableReason::StaleRevision)),
                      "a changed Responses revision is not retried after progress");
    failures += check(!checkpoint_refusal_is_transient(
                          outcome(DurableReason::SessionNotIndexed)),
                      "an evicted session is retried as if it could still be exported");
    failures += check(!checkpoint_refusal_is_transient(
                          outcome(DurableReason::UnsupportedCodec)),
                      "a structural program refusal is retried");
    failures += check(!checkpoint_refusal_is_transient(
                          outcome(DurableReason::Quota)),
                      "a quota refusal is retried");
    auto authoritative = outcome(DurableReason::Capacity);
    authoritative.reason = SessionCheckpointSkipReason::TransactionBusy;
    failures += check(!checkpoint_refusal_is_transient(authoritative),
                      "legacy coarse diagnostics overrode an authoritative K7 capacity refusal");

    // Graceful stop accounting.
    SessionCheckpointSkipDetail none;
    failures += check(classify_shutdown_checkpoint(false, true, none) ==
                          ShutdownCheckpointOutcome::Saved,
                      "a saved session is not counted as saved");
    failures += check(classify_shutdown_checkpoint(
                          true, false, outcome(DurableReason::SessionNotIndexed)) ==
                          ShutdownCheckpointOutcome::NothingToSave,
                      "an evicted session whose newest response is on disk is counted as lost");
    failures += check(classify_shutdown_checkpoint(
                          false, false, outcome(DurableReason::SessionNotIndexed)) ==
                          ShutdownCheckpointOutcome::Refused,
                      "an evicted session whose newest response never reached disk is not a loss");
    failures += check(classify_shutdown_checkpoint(
                          false, false, outcome(DurableReason::NoSessionRecords)) ==
                          ShutdownCheckpointOutcome::NothingToSave,
                      "a session without stored responses is counted as lost");
    failures += check(classify_shutdown_checkpoint(
                          false, false, outcome(DurableReason::CacheDisabled)) ==
                          ShutdownCheckpointOutcome::NothingToSave,
                      "a server without a context cache reports a loss");
    failures += check(classify_shutdown_checkpoint(
                          false, false,
                          outcome(DurableReason::UnsupportedCodec)) ==
                          ShutdownCheckpointOutcome::Refused,
                      "a program refusal of live state is not a loss");

    // Retries are bounded per turn, and the budget holds only sessions whose retry is still
    // queued: spending every retry or settling releases the entry, so session churn cannot grow
    // it (council CR-20260924-ninfer-durable-evict-r1, daybreak-blue R3).
    ninfer::serve::TransientRetryBudget budget;
    const std::string first(64, 'a');
    const std::string second(64, 'b');
    unsigned granted = 0;
    while (granted <= ninfer::serve::kTransientCheckpointRetries && budget.retry(first)) {
        ++granted;
    }
    failures += check(granted == ninfer::serve::kTransientCheckpointRetries,
                      "a turn's transient refusals were not retried exactly the bounded number");
    failures += check(budget.size() == 0, "a session whose retries are spent still holds an entry");
    failures += check(budget.retry(second) && budget.size() == 1,
                      "a queued retry is not held until it settles");
    budget.settle(second);
    budget.settle(second);
    failures += check(budget.size() == 0, "settling a session did not release its entry");

    // Automatic saves skip short sessions, but a session with a generation on disk keeps it
    // current at any size: after a compaction its new lineage starts below the minimum, and
    // skipping it left a crash restoring the pre-compaction generation.
    using ninfer::serve::checkpoint_worth_saving;
    failures += check(!checkpoint_worth_saving(7'400, 32'768, false),
                      "a short session with nothing on disk was saved");
    failures += check(checkpoint_worth_saving(32'768, 32'768, false),
                      "a session at the minimum was not saved");
    failures += check(checkpoint_worth_saving(24'700, 32'768, true),
                      "a compacted session's on-disk generation was left stale");

    if (failures == 0) { std::cout << "checkpoint policy tests passed\n"; }
    return failures == 0 ? 0 : 1;
}
