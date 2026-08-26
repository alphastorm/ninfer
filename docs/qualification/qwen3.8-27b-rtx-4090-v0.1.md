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
| Release ID | `qwen38-4090-v0.1-66e08ca64122` |
| Executable source commit | `66e08ca641223d8467d4ec1e6ab1029028062cac` |
| Upstream base | `9ec1b82c7afa021314682d7a95390f8935ead7c2` |
| Source tree at build | clean |
| Target | native Windows x86-64, Ada `sm_89` |
| Build | Release, MSVC 19.44.35228.0, CUDA 13.3.73 |
| Model source | `neroued/Qwen3.8-27B-NInfer@18dfc887423fa5aabf3cb56fac41490e462b3fab` |
| Model artifact SHA-256 | `eec39564993d6e9c7d5e383382a760f093465c9d163ec9a1bd6b80199514bf3e` |
| Model artifact bytes | `18,210,531,328` |
| Config SHA-256 | `8ee93f07172d9dd5044b4fa3a8823a53b7ce616d890b5352a44f18be4e901c5f` |
| Server binary SHA-256 | `aa2bc9b8929e6f9085caa5501f1b9553701c1bfb51f4b277691b528b0b293a1b` |

The documentation commit containing this record follows the executable source commit and is not part
of the package identity.

## Package integrity

| Item | Value |
|---|---|
| Archive | `ninfer-qwen38-4090-v0.1-windows-x86_64.zip` |
| Archive SHA-256 | `5d873b9a5fa910b564e3a5e72df7cc8c96e984047e8329ec787173882206fd3a` |
| Archive bytes | `228,992,663` |
| Manifest entries | 22, all verified |
| Runtime DLLs | 12 |
| Private source receipt SHA-256 | `b554476c3f3d9e0dbd353f2e65a7e7b391b91a4d621b10d983d1f7103e19846b` |

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
| Elapsed time | 88.4687637 s | 4.9079209 s |
| Restored compatible tokens | — | 105,068 |
| Prepare time | — | 0.0722435 s |
| Reuse path | — | `restore_disk_checkpoint` |

## Source provenance

| Evidence | Identity |
|---|---|
| Qualified raw patch SHA-256 | `ac8ba36da9bfc89ff97021eecba8c7ba8f88f9e7e9bfd1c9c3509b723d81d4e0` |
| Normalized patch SHA-256 | `6cf03d7b4562165d9c7416b0878ff183dae0db00811176ead11e487f1c916898` |
| Patch series SHA-256 | `7da9a7b244275fb306bcaf745b61ed183c60c8a85fdf272e640ba3cb95aa71a3` |
| Bounded staging commit | `e89e415d95921cecf4cf8a00df5540a4a036ff84` |
| DirectStorage fallback commit | `a9bb55b8e9b02ff34e4725dc8ca95d4fadeb3e0d` |

## Isolation and sanitization

Qualification used isolated candidate resources. Candidate runtime state was removed afterward, the
incumbent remained available, and no production service, route, lock, or pin was modified. No tag,
push, or artifact publication occurred.

The public JSON omits private build/deployment profile identifiers. Neither public file contains host
names, private paths, endpoint addresses, credentials, raw prompts, generated text, session IDs, or
private route/lock data.
