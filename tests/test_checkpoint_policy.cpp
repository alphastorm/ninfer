// Which refused saves an automatic checkpoint retries, how many times, and how a graceful stop
// accounts a session whose engine continuation is gone.
#include "serve/checkpoint_policy.h"

#include <iostream>
#include <string>

namespace {

using ninfer::runtime::ContinuationExportSkipReason;
using ninfer::runtime::SessionCheckpointSkipDetail;
using ninfer::runtime::SessionCheckpointSkipReason;
using ninfer::serve::ShutdownCheckpointOutcome;

int check(bool condition, const char* message) {
    if (condition) { return 0; }
    std::cerr << message << '\n';
    return 1;
}

// What the checkpoint store reports when it replaced an engine gate with the generic refusal.
SessionCheckpointSkipDetail masked(SessionCheckpointSkipReason engine_gate) {
    SessionCheckpointSkipDetail skip;
    skip.reason       = SessionCheckpointSkipReason::ProgramRejected;
    skip.first_reason = engine_gate;
    return skip;
}

SessionCheckpointSkipDetail program_gate(ContinuationExportSkipReason gate) {
    SessionCheckpointSkipDetail skip;
    skip.reason               = SessionCheckpointSkipReason::ProgramRejected;
    skip.export_detail.reason = gate;
    return skip;
}

SessionCheckpointSkipDetail plain(SessionCheckpointSkipReason reason) {
    SessionCheckpointSkipDetail skip;
    skip.reason = reason;
    return skip;
}

} // namespace

int main() {
    using ninfer::serve::checkpoint_refusal_is_transient;
    using ninfer::serve::classify_shutdown_checkpoint;
    using ninfer::serve::effective_checkpoint_skip_reason;
    int failures = 0;

    failures += check(effective_checkpoint_skip_reason(masked(
                          SessionCheckpointSkipReason::SessionNotIndexed)) ==
                          SessionCheckpointSkipReason::SessionNotIndexed,
                      "the engine gate the store replaced is not the effective reason");
    failures += check(effective_checkpoint_skip_reason(program_gate(
                          ContinuationExportSkipReason::EndpointNotRetained)) ==
                          SessionCheckpointSkipReason::ProgramRejected,
                      "a genuine program refusal lost its reason");

    // The two refusals EXP-049 observed while the sessions were still resident: early, not final.
    failures += check(checkpoint_refusal_is_transient(masked(SessionCheckpointSkipReason::TagMismatch)),
                      "a save that ran before the new continuation was catalogued is not retried");
    failures += check(checkpoint_refusal_is_transient(
                          masked(SessionCheckpointSkipReason::TransactionBusy)),
                      "a save that ran during another request's transaction is not retried");
    failures += check(checkpoint_refusal_is_transient(
                          program_gate(ContinuationExportSkipReason::ContextTransactionBusy)),
                      "a program-level transaction refusal is not retried");
    failures += check(!checkpoint_refusal_is_transient(
                          masked(SessionCheckpointSkipReason::SessionNotIndexed)),
                      "an evicted session is retried as if it could still be exported");
    failures += check(!checkpoint_refusal_is_transient(
                          program_gate(ContinuationExportSkipReason::EndpointNotRetained)),
                      "a structural program refusal is retried");
    failures += check(!checkpoint_refusal_is_transient(
                          plain(SessionCheckpointSkipReason::QuotaExceeded)),
                      "a quota refusal is retried");

    // Graceful stop accounting.
    SessionCheckpointSkipDetail none;
    failures += check(classify_shutdown_checkpoint(false, true, none) ==
                          ShutdownCheckpointOutcome::Saved,
                      "a saved session is not counted as saved");
    failures += check(classify_shutdown_checkpoint(
                          true, false, masked(SessionCheckpointSkipReason::SessionNotIndexed)) ==
                          ShutdownCheckpointOutcome::NothingToSave,
                      "an evicted session whose newest response is on disk is counted as lost");
    failures += check(classify_shutdown_checkpoint(
                          false, false, masked(SessionCheckpointSkipReason::SessionNotIndexed)) ==
                          ShutdownCheckpointOutcome::Refused,
                      "an evicted session whose newest response never reached disk is not a loss");
    failures += check(classify_shutdown_checkpoint(
                          false, false, plain(SessionCheckpointSkipReason::NoSessionRecords)) ==
                          ShutdownCheckpointOutcome::NothingToSave,
                      "a session without stored responses is counted as lost");
    failures += check(classify_shutdown_checkpoint(
                          false, false, masked(SessionCheckpointSkipReason::CacheDisabled)) ==
                          ShutdownCheckpointOutcome::NothingToSave,
                      "a server without a context cache reports a loss");
    failures += check(classify_shutdown_checkpoint(
                          false, false,
                          program_gate(ContinuationExportSkipReason::EndpointNotRetained)) ==
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

    if (failures == 0) { std::cout << "checkpoint policy tests passed\n"; }
    return failures == 0 ? 0 : 1;
}
