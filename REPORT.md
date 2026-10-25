# Integration checkpoint — not qualified

- Branch: `rebase/upstream-64d10fbf`; code/API boundary through `5a15de35`, authoritative contract prose through `6b91d39ea` plus the exact Main-approved Windows K6 clarification from `23c65039`. This handoff does not close the release integration acceptance criteria. [INTEGRATION.md](INTEGRATION.md) contains the merge log, immutable slice-report links, shared-file requests, baseline disposition and conflict watchlist.
- Base dev commits `9822c38d`, `4ec903a0`, `1e902680` each conflicted or crossed master-specific boundaries; grammar, artifact I/O and container re-expression were routed to Frontend, PortableCore and Infra, respectively. No unsafe wholesale cherry-pick was made.
- Shared prerequisites include the single native architecture profile gate, private/principal scope carriers, semantic reuse-path/occupancy declarations, K5 identity/durable/managed-stop options, internal adapter friendship, Function/Custom tool kinds and schema-mismatch reason. These declarations do not prove the later runtime implementations.
- Frozen Contracts `746b29d2` merged as `a2fcef5c`; startup identity getters are `42efad145`. Follow-ups: K7 reason helper `7f51c3a96`, trusted pressure/oracle/progress declarations `9eb0a5fb9`, retryable prepare/import `Unavailable` states `5a15de35`. Tests contain signature/spelling assertions; these latest changes have not been rebuilt.
- Main-approved Reader failure/fresh-verifier and pinned-reader oracle changes were authored as exact shared-header patches and authorized on the DurableStore/Persistence/B2 provider copies, because `continuation_checkpoint.h` is not yet on this base. The final contract uses `fresh_reader()` and `pin_recoverable()`, not a bool or optional id/epoch ABI. Provider implementations and every fake/caller must migrate together.
- Executed integration job `20261025T015729Z-int-checkpoint-36a42fac-336877`: exact source `36a42facfec7280265fa929fa78044136797d70d`, image `sha256:e7f34ca2f41ecea14da5cce0b2a30cae6b0877e3b3eb371bd92d103bca1121f1`, exit 1 after 72 s, zero leftover/unowned containers. Compilation failed at `tests/test_durable_contract_headers.cpp:5` because its target lacked the pinned JSON include interface. **No host or model test ran.** `c47762e9` fixes the registration with `target_link_libraries(... PRIVATE ninfer::json)`; not rerun at this handoff.
- Earlier executed integration prerequisite `dc3140b23` built apps/tests for `120a`; lease elapsed 289 s, exit 0, `ninfer_qwen_frontend_test` passed in 0.71 s. This proves the earlier carrier prerequisite only, not the later union.
- Consumed Infra evidence: vanilla apps/tests build 285.323 s; corrected-image aggregate host 45/45; real-model 7 pass / 6 fail / 2 skip, plus explicit dense v3 loading pass. Three artifact-specific exclusions are explicitly recorded; grammar, continuation and prefix were never silently excluded. Published v3 payload/resource equivalence is fully recorded by Infra.
- Consumed mechanical G1 receipt `0e64cc77/G1.json`: source `e442df4d`, real restart, 22,010 hit tokens, 93 byte-identical continuation bytes, 64 s. Its C1/text/32K tracer scope is not production checkpoint-set, Responses, C2, Windows or A3 qualification.
- Integrated isolated repairs: no-EOS fixture policy `1bcaffa4d` and rewind reservation `36a42facf`. Reader-after-admission reservation `d1f4832e` and cause-bound prefix golden `a072d149b` remain next-checkpoint inputs; no GPU repair verdict follows from the failed compilation.
- CPU-ready providers: Frontend `8c3102905` and ServePort `141839f8`. DurableStore **withdrew** `550fe0069` / tested `3c1d84343` for catalog-lock/publication-gate inversion; replacement header `5b2f9b75c` and implementation/regressions `202b17064` are pushed, but their new host proof has not run. Main permits B2's own source union, explicitly unqualified; its `99a6b0991` consumes the frozen Reader/Unavailable boundary, not a provider-qualified base. ServePort2 reports its first host attempt stopped at duplicate `ninfer_sha256` configuration; scoped deduplication is authorized, not tested here.
- EngineCache `809365a96` is not merge-qualified: its `5a21cefe8` proof failed compilation before runtime tests, its `ninfer_core` include-dependency repair is unrebuilt, and production A3 bodies are not implemented. This blocks B2 compile/link/restart completion; no fake adapter is acceptable. Persistence2's tracer cleanup and native regressions are separate provider work, not integrated GPU proof.
- Native Windows builds, both native GPU lanes, G2 parity, full G3, G4 review and release composition remain unproven here. NativeOps/ArchEnable2/PortableCore own their pending combined-source receipts. Infra successor `8700b2b21` supplies supervised container cleanup; never reinstall older helpers over the deployed common copies.
- Next authorized integration action: consume the corrected provider boundaries and prerequisites, preserve the current K5/K0 union, build once and run all host tests plus the full applicable dense real subset including the corrected prefix case. Expand selectors for newly registered tests. Tag only after required results pass. No green checkpoint, production change, publication, local build/test, formatter or linter is claimed.

## Executed build command boundary

These commands are from the submitted checkpoint script. Its private roots are represented by the original variable names rather than committed host paths.

```sh
export NINFER_BUILD_HOLDER=int
export NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2
git -C "$source_root" fetch origin rebase/upstream-64d10fbf
git -C "$source_root" checkout --detach "$revision"
git -C "$source_root" rev-parse HEAD
docker image inspect "$NINFER_BUILD_ENV_IMAGE" --format '{{.Id}}'
"$common/build.sh" "$source_root" "$build_root"
```

The cancelled predecessor remained queued and produced no execution evidence. The failed job's planned 47-host / nine-real / KV-regression / explicit-loader phases did not execute. Host tests require the real driver with an empty device mask; actual server/GPU proofs leave `CUDA_VISIBLE_DEVICES` absent. No Integrator lease job remains active at handoff.
