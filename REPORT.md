# Persistence slice

- Base: upstream `64d10fbf`; branch `rebase/persistence`.
- RepoPrompt preparation: `prep-v0-13-0-upstream-re-DF08C8`; export `oracle-plan-2026-10-24-174946-prep-v0-13-0-upstrea-1d64.md` (private, not committed).
- G1 tracer re-expressed on `CheckpointStore::Lease`, `StateImageStore::ReadLease`, `ContextSnapshot`, `KVHistoryStore` and native sequence mapping/commit helpers. Full StateImage includes the DFlash2 ring; settled ring frontier is required.
- G1 is a bounded experiment: one BF16/DFlash2 lane, text-only full-history Chat continuation, authenticated disk payload, real process restart, fresh native ownership. No Responses-store, multi-point, Host-only, concurrent, Windows or performance claim follows from this probe.
- Input: fork-owned tracer at `49bc4219`; community checkpoint codec read as design reference only, no community source copied.
- Engine hooks honor `allow_prefix_reuse`, so startup warmup never attempts import. Other architecture Programs are not given a fake persistence implementation.
- Launcher confines containers to holder `A2` and loopback port `18120`; receipts contain names, hashes and counters, not host paths, model text or credentials.
- Verification: not yet run; no G1 verdict. Phase 2 begins after Contracts freezes K1/K3.
