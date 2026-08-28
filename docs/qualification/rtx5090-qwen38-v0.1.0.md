# RTX 5090 × Qwen3.8 v0.1.0 qualification

**Decision as of 2026-08-26: PASSED on the exact recorded candidate; external publication and promotion remain unauthorized.**

The machine-readable authority is [`rtx5090-qwen38-v0.1.0.json`](rtx5090-qwen38-v0.1.0.json). The schema-valid bounded measurement is [`fixtures/rtx5090-qwen38-v0.1.0-measurement.json`](fixtures/rtx5090-qwen38-v0.1.0-measurement.json). No hostname, private path, container ID, API secret, request/session ID, prompt, generated Golden data, or content-bearing request log is included.

## Qualified identity

| Field | Value |
|---|---|
| target | Qwen3.8-27B `groupwise-int` on one RTX 5090 |
| GPU / architecture | NVIDIA GeForce RTX 5090 / `sm_120a` |
| driver | `610.88` |
| runtime source | `70868c658f5bd412ead5b105ec76939997bd6ca9` |
| upstream base | `4eef14a7560d87a3ba717898e1d488a4c4c7246d` |
| image ID | `sha256:3bb6b96aaa5bb7c7345fb90caf8ea5cd3c7569f22e855be9adaed34db1cd5efb` |
| `ninfer-serve` SHA-256 | `d54af118458d820955c6bcf53bdab7a80799343c68a37bcce7f2837f85e559e0` |
| model SHA-256 | `eec39564993d6e9c7d5e383382a760f093465c9d163ec9a1bd6b80199514bf3e` |
| config SHA-256 | `c6b635ccab19fd712ba503cd051b5359bb9c355068706b17372860baa0de49b6` |
| toolchain | GNU 13.3.0; CUDA compiler/toolkit 13.1.115 |

Runtime profile: max context 131,072; BF16 KV; prefill chunk 1,024; one active request; MTP3 with draft LM head; Vision and thinking preservation enabled; loopback binding; Docker restart policy `no`.

## Gate results

| Gate | Result |
|---|---|
| serving contract | passed OpenAI, Anthropic, Responses, count-tokens, and image-token smoke |
| Responses identity/lifecycle | passed first turn, continuation, two forks, content-safe cross-session 404, parent deletion, and surviving descendants |
| exact long context | 130,048 prompt tokens; exact `ORCHID=493817; COLOR=COBALT`; 58.546667 s |
| Golden t01 | exact oracle passed; 100.227 s ≤ 120.9117 s |
| decode | 1,143 committed tokens / 5.467908612 s = 209.0378756 tok/s ≥ 200.58921 |
| MTP acceptance | 799 / 1,038 = 0.7697495, inside [0.72319, 0.78319] |
| cache reuse | same-session `private_response_replay`; isolated session `root`; 18 content-free telemetry cache-hit requests |
| process exit | remained `exited` for the five-second grace; restart-count delta 0; explicit re-promotion healthy |
| rollback | candidate removed; exact incumbent restored with authenticated status `ok`; previous rollback runtime remained stopped |

Decode/MTP used one committed `long_decode_aime26_01` fixture, one fixed seed, one measured request, and server-reported phase/counter telemetry. The service was warm from preceding protocol and Golden gates. Both thresholds passed on the first measured request; no retry or broad benchmark campaign ran.

Golden used OMP 18.0.5 through the approved external t01 workload. Only the fixture digest, exact-oracle decision, and elapsed time are published. The prompt and output remain private.

## Source and evidence boundary

The executable identity is the clean runtime source commit `70868c658f5bd412ead5b105ec76939997bd6ca9`. This qualification document necessarily postdates execution. Its documentation commit is evidence metadata, not a claim that a binary rebuilt from that later commit was tested. A source tag and binary package must bind to the runtime source SHA above; changing executable input or its embedded patch-stack identity requires fresh qualification.

## Scope and disposition

This is one machine and one exact profile, not a universal performance claim. The candidate container was removed after qualification; its image and private bounded receipts remain available for diagnosis. The exact incumbent was restored healthy. No route, lock, tag, push, registry upload, model publication, or remote release occurred.
