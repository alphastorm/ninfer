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
| `evidence_parent` | Controller-side mutable evidence parent, disjoint from every immutable input |
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

Before creating an attempt directory, marker or orchestrator, the driver compares its evidence
parent and attempt directory, builder/target attempt roots and managed scratch root against all
configured immutable inputs, including the configuration file, checkout, fixtures, toolchains,
inventories, model, API key, OMP fixture, incumbent namespace and qualified runtime/build receipt.
Overlap in either direction is refused without creating attempt artifacts. Windows paths use
one case-insensitive absolute namespace, including relative controller paths and drive roots.

The five groups execute all 15 existing phases in their canonical order. Each successor group
also re-runs preflight under its own lock: preceding cleanup may have restarted the incumbent,
so the new window must re-establish target quiescence and its write-ahead restoration obligation.
Completed build, protocol, pressure and timing measurements are not replayed. Select the
registered card lock for ordinary groups and the timing-isolation lock required by the security
gate. Every group restores before releasing its lock; the final group retires the exact scratch
installation/task after restoration. The underlying orchestrator's gates and public lane receipt
schema are unchanged. Lane composition validates a completed projection of persisted phase
evidence. Native state is saved as passed only after lane and final group publication complete;
publication failure records failed state instead. A failed restoration is not retried implicitly,
and stage failure is persisted before cleanup so a cleanup exception cannot leave a running stage.

For a deliberately paused chain, `--continue-from security` (or another group) requires every
preceding group's completed, state-bound receipt, unchanged config/tool bytes and matching
phase receipt hashes. Tool binding includes the chain, shared `driver_state.py` and native
orchestrator modules. The native constructor loads and validates prior state independently of
`resume`; the driver keeps `resume=False` so only explicitly selected phases execute, rather
than checkpoint-skipping a newly entered group. It does not rerun protocol or pressure tests.
A started/failed group is not retryable under the same attempt. Preserve it, diagnose the cause
and register a fresh attempt; do not remove `.started` files or manufacture completion receipts. In particular,
metadata-only finalization is not permission to replay a measured phase.

## Upgrade rehearsal

Stage these two repository files together on the Windows executor:
`packaging/windows/Invoke-UpgradeRehearsal.ps1` and `Invoke-UpgradeInstall.ps1`.
The controller runs all three scenarios under the supplied lock command:

```sh
python -B packaging/windows/run_upgrade_rehearsal.py --config "$UPGRADE_DRIVER_CONFIG" --attempt "$ATTEMPT"
```

The controller config supplies `evidence_parent`, `immutable_roots` (input directories),
`lock_command` (argument array), `command` (transport argument array ending with the staged
PowerShell driver and `-Config` plus its private Windows config filename), and
`timeout_seconds`. Use a stdout-preserving, exit-status-preserving transport. Its logs and
working directory must be outside immutable input roots; do not let a transport create the
fresh attempt root before the driver. The Windows driver also refuses overlapping input/output
roots, including every configured read-only canonical root. Keep the outer transport deadline
beyond the scenario's installer and cleanup budgets;
retain the release window's independent supervisor/restoration guard on transport loss.

The Windows config supplies:

- `output_parent`, `state_root_leaf`, `task_name`: registered scratch parent and the lane's
  specification leaf/task base. Each scenario claims its own `<attempt>-<scenario>` root.
- `incumbent` and `candidate`: objects containing `installer`, `installer_sha256`, `package`,
  `package_sha256`, `running_power_limit_w`; candidate also supplies the exact `instance_id` declared by its release ID
  and source prefix. Freeze these against the authenticated component closure, not filenames.
- `model`, `api_key_file`, `gpu_uuid`, `port`, `owner_power_limit_w`: explicit registered inputs.
- `canonical_roots`: nonempty read-only census list; state/task XML/lease presence must remain
  unchanged. Canonicals are never installed, stopped, renamed or deleted by the driver.

Use an attempt of at most 23 safe characters for the upgrade task suffix. The Windows host
must have an elevated Windows PowerShell, the unchanged package lifecycle helpers beside each
installer, the actual predecessor assets, model/key, and the existing native lane prerequisites.
The scratch volume must retain the unchanged 60 GiB free-space floor. Each installer child has
a 900-second limit; logs and transaction evidence are retained. Running observations retain
the predecessor/candidate power and inner/outer owner-lease assertions; candidate effective
Device and Host weight counters must both be positive.
The three scenarios are:

1. **running**: install and start the real predecessor, upgrade it, require candidate health,
   exact active instance/package identity, predecessor pointers and a completed running-handoff transaction.
2. **rollback**: open an exclusive loopback listener immediately before the candidate's first
   Start. Release it at rollback entry. Require the specific occupied-port failure, completed
   rollback, deletion of the four transaction-created trees and healthy exact predecessor,
   including its saved previous/prepared lifecycle pointers.
3. **repair**: pause after durable candidate activation. The parent verifies the persisted
   journal, active instance and owned child PID before interruption. Invoke the unchanged
   `-RepairInterruptedInstall`, require repair/cleanup and healthy predecessor, then reinstall
   the same instance successfully.

Fault injection uses hash-pinned installer bytes and unique source anchors, not fixed line
numbers, edited packages, a fabricated server or a shipped production fault hook. A missing
or ambiguous anchor refuses the attempt. These scenarios do **not** claim to test a candidate
server's failed Stop, access-denied Kill, an inherited paused owner, or every crash interval.

Managed cleanup repairs interrupted transactions with the initiating installer, including
incumbent transactions before state or instance identity exists. It stops only the scratch
installation when state exists, unregisters its exact task even without state, and checks
process/lease/task absence, original power and unchanged canonicals. Independent cleanup steps
continue after an assertion fails; terminal state retains cleanup errors without masking the
primary scenario exception. Failure never publishes success. Completed state precedes the
state-hashed receipt; controller continuation consumes only completed receipts.
Native transport accepts a complete pretty JSON object or a
terminal compact receipt after progress text; the upgrade compact boundary explicitly rejects
pretty/multiline output. Neither parser searches inside a pretty object for a nested receipt.

## Evidence and privacy

All driver directories and raw qualification receipts are private window evidence. Keep keys,
SSH configuration, host inventory, absolute execution roots, GPU identifiers and raw lifecycle
logs outside the repository. Publish only the product's reviewed, privacy-sanitized lane receipt
and neutral driver result; never copy the entire evidence tree into a PR or release. Preserve
failed attempts and refer to them by neutral identifiers in public reports.

Upgrade-rehearsal evidence must bind exact rollback previous/prepared pointers and the running
candidate identity. Rehearsal output must exclude canonical-root descendants, interruption
markers must be created atomically, and failed attempts must retain transport logs together
with independent cleanup and final-state evidence. Successful census receipts use an array of
rows; failed rehearsal census preserves `rows: null` plus its error instead of inventing an
empty census.

The GPU-free `release-drivers` Actions job runs the invariant suite on Linux and Windows,
including namespace, JSON boundary, receipt ordering, PowerShell parsing/anchor checks, the
three rehearsal scenario transitions and native chain failure/recovery checks with fake
providers. The chain boundary regressions retain the real native constructor, lane loader, state loader,
phase publisher, preflight and restoration logic while replacing external providers.
Lane specification/configuration fixtures use the repository's complete lane schemas
so newly required fields propagate to every real-orchestrator boundary test.
No CTest selection or numerical/acceptance tolerance is changed.
