# Qwen3.8-27B RTX 4090 v0.1 qualification

This record covers the immutable native Windows package qualified on an NVIDIA GeForce RTX 4090.
It is a **beta qualification**, not a publication announcement. The protocol (`P`) and installed-package
NInfer restart (`L`) gates passed; the private Golden (`G`) task and the OMP-restart portion of `L`
were not run in this lane.

The machine-readable record is
[`qwen3.8-27b-rtx-4090-v0.1.json`](qwen3.8-27b-rtx-4090-v0.1.json).

## Qualified release tuple

| Item | Qualified value |
|---|---|
| Release ID | `qwen38-4090-v0.1-ea265776254a` |
| Executable source commit | `ea265776254a62ab5184454ba0163cdf04aad1e5` |
| Upstream base | `9ec1b82c7afa021314682d7a95390f8935ead7c2` |
| Source tree at build | clean |
| Target | native Windows x86-64, Ada `sm_89` |
| Build | Release, MSVC 19.44.35228.0, CUDA 13.3.73 |
| Model source | `neroued/Qwen3.8-27B-NInfer@18dfc887423fa5aabf3cb56fac41490e462b3fab` |
| Model artifact SHA-256 | `eec39564993d6e9c7d5e383382a760f093465c9d163ec9a1bd6b80199514bf3e` |
| Model artifact bytes | `18,210,531,328` |
| Config SHA-256 | `d613fc71ebe30b799af4936af8f73b0f25ebd1a1486b55fdb59aaab5d884bb96` |
| Server binary SHA-256 | `19b6f0bec37040c71b40b7df6213ab762004c8b4a8b039db736064d686fbf404` |

The documentation commit containing this record follows the executable source commit and is not part
of the package identity.

## Package integrity

| Item | Value |
|---|---|
| Archive | `ninfer-qwen38-4090-v0.1-windows-x86_64.zip` |
| Archive SHA-256 | `acd931e26429880560af3c8a7b5b61aeef8baafe0572fbadeed7aff540547530` |
| Archive bytes | `229,003,945` |
| Manifest entries | 22, all verified |
| Runtime DLLs | 12 |
| Private qualification receipt SHA-256 | `ef0c05d4a10974db626c35590b88ab50d720c689a537ec413008c984856fe42b` |

The package was installed from the immutable archive into isolated candidate state before
qualification. It has not been published.

## Runtime profile

```text
Qwen3.8-27B groupwise-int
131,072-token context
rk2v4-e8 KV
prefill chunk 512
MTP0 (speculation disabled)
xhigh reasoning effort
100 GiB disk cache
concurrency 1
```

MTP0 is the selected qualified profile. The bounded, otherwise-identical MTP3 comparison was **not
attempted** because the private Golden gate was not run. Consequently, this record makes no MTP3
promotion or throughput claim.

## P/L/G gates

| Gate | Verdict | Exact scope |
|---|---|---|
| **P** | **Passed** | Installed-package protocol and tool correctness; 10 passed, 0 failed. |
| **L** | **Passed with stated boundary** | 100K+ continuation across NInfer restart restored from disk; the OMP-restart component was not run. |
| **G** | **Not run** | No real Golden implementation task was executed in this lane. |

`P` covered authenticated status, Anthropic token counting, typed tool arguments, text-part tool-result
arrays, duplicate tool-name rejection, malformed identity rejection, fail-closed unsupported
structured output, Responses continuation, cross-session isolation, and streaming parity.

## Persistence evidence

| Measurement | Cold request | After NInfer restart |
|---|---:|---:|
| Prompt tokens | 105,052 | 105,089 |
| Completion tokens | 16 | 16 |
| Elapsed time | 88.0348313 s | 16.1876186 s |
| Restored compatible tokens | — | 105,068 |
| Prepare time | — | 0.0977603 s |
| Reuse path | — | `restore_disk_checkpoint` |

## Source provenance

| Evidence | Identity |
|---|---|
| Qualified raw patch SHA-256 | `ac8ba36da9bfc89ff97021eecba8c7ba8f88f9e7e9bfd1c9c3509b723d81d4e0` |
| Normalized patch SHA-256 | `6cf03d7b4562165d9c7416b0878ff183dae0db00811176ead11e487f1c916898` |
| Patch series SHA-256 | `7da9a7b244275fb306bcaf745b61ed183c60c8a85fdf272e640ba3cb95aa71a3` |
| Bounded staging commit | `e89e415d95921cecf4cf8a00df5540a4a036ff84` |
| DirectStorage fallback commit | `a9bb55b8e9b02ff34e4725dc8ca95d4fadeb3e0d` |
| Configured-source identity verifier | passed before every compile target |
| Dirty source after configure | build rejected, exit 1 |
| Restored clean source | build target passed |

## Isolation and sanitization

Qualification used isolated candidate resources. The incumbent was temporarily stopped to release
the GPU, then restored and verified after candidate state, task, listener, and private qualification
inputs were removed. No production service configuration, route, lock, or pin was modified. No tag,
push, or artifact publication occurred.

The public JSON omits private build/deployment profile identifiers. Neither public file contains host
names, private paths, endpoint addresses, credentials, raw prompts, generated text, session IDs, or
private route/lock data.
