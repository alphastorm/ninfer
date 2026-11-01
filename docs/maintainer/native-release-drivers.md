# Native release window drivers

Use the repository drivers, not copied per-release wrappers, for the next native lane window.
These commands perform real Windows installation and GPU qualification. CI exercises only
file-only invariants and fake lifecycle providers; green CI is not hardware qualification or
permission to acquire a window. Pin the driver commit and register the candidate/predecessor,
model, ports, lock resources, scratch roots, deadlines and restoration evidence before running.

## Native qualification chain

```sh
python -B tools/qualification/run_native_chain.py --config "$NATIVE_DRIVER_CONFIG" --attempt "$ATTEMPT"
```

The external JSON config has these keys:

| Key | Contract |
|---|---|
| `evidence_parent` | Controller-side mutable evidence parent, outside the source checkout |
| `builder_parent`, `target_parent` | Existing protected Windows scratch parents; the driver exclusively creates `<attempt>-builder` and `<attempt>-target` beneath them |
| `expected_package_sha256` | Exact frozen public candidate package digest; the qualification repack must match it |
| `lock_commands` | Object with `cpu`, `protocol`, `correctness`, `security`, `finish` argument arrays; each command holds the registered lock while running the appended child command |
| `native` | Arguments for `qualify_native.Config`, using CLI names with underscores; see the existing orchestrator's `--help` |

`native` supplies `source_root`, `lane`, `builder`, `target`, `builder_vcpkg`,
`builder_cuda_compiler`, `builder_cxx_compiler`, `model_path`, `long_fixture`, `omp_root`,
`transfer_tool`, `incumbent_state_root`, `candidate_source`, `dependency_sbom_tool`,
`vcpkg_resolved`, `cuda_inventory`, `third_party_inventory`, `runtime_inventory`,
`scratch_state_root`, `scratch_port`, `pressure_port`, `api_key_file`,
`qualified_runtime_bin`, `qualified_build_receipt`, and optionally `gpu_uuid`.
Use JSON null for unused optional inputs. Qualified runtime/bin and receipt inputs select
prebuilt-only operation; do not supply compiler inputs to imply an approved build window.

Strings may contain `{attempt}`. The managed scratch root must be beneath the newly claimed
`<attempt>-target` directory and retain the lane specification's required leaf. The driver owns
`state_dir`, `scratch_task_suffix` (equal to the attempt), `resume`, `dry_run`, `through_phase`
and `upgrade_orchestrator_state`; do not supply alternate state/resume policies.
Hosts are SSH aliases; SSH config outside the repository supplies ports, users and credentials.
The dependency inventories, API key and OMP fixture must already be staged at registered paths.
No private bootstrap script or source patch is required.

The five groups execute all 15 existing phases in their canonical order. Select the registered
card lock for ordinary groups and the timing-isolation lock required by the security gate.
Every group restores before releasing its lock; the final group retires the exact scratch
installation/task after restoration. The underlying orchestrator's gates and public
lane receipt schema are unchanged. Qualification state is saved as passed before lane receipt
composition; the composer receives persisted state, not a future in-memory claim.

For a deliberately paused chain, `--continue-from security` (or another group) requires every
preceding group's completed, state-bound receipt, unchanged config/tool bytes and matching
phase receipt hashes. It does not rerun protocol or pressure tests. A started/failed group is
not retryable under the same attempt. Preserve it, diagnose the cause and register a fresh
attempt; do not remove `.started` files or manufacture completion receipts. In particular,
metadata-only finalization is not permission to replay a measured phase.

## Evidence and privacy

All driver directories and raw qualification receipts are private window evidence. Keep keys,
SSH configuration, host inventory, absolute execution roots, GPU identifiers and raw lifecycle
logs outside the repository. Publish only the product's reviewed, privacy-sanitized lane receipt
and neutral driver result; never copy the entire evidence tree into a PR or release. Preserve
failed attempts and refer to them by neutral identifiers in public reports.

The GPU-free `release-drivers` Actions job runs the invariant suite on Linux and Windows,
including namespace, JSON boundary, receipt ordering and native chain failure/recovery checks
with fake providers. No CTest selection or numerical/acceptance tolerance is changed.
