# HTTP serving

`build/apps/ninfer-serve` loads one registered artifact and exposes OpenAI- and
Anthropic-compatible HTTP endpoints over one resident NInfer Engine.

## Start the server

```bash
./build/apps/ninfer-serve models/qwen3_8_27b_nvfp4.ninfer \
  --host 127.0.0.1 \
  --port 8080 \
  --max-context 240000 \
  --kv-capacity 240000 \
  --max-concurrency 2 \
  --kv-dtype fp8 \
  --device-state-slots 2 \
  --host-state-slots 8 \
  --host-kv-mib 8192 \
  --spec mtp --draft-tokens 3 \
  --lm-head-draft \
  --preserve-thinking
```

The command uses Qwen3.8-27B NVFP4. Each request has a 240,000-token logical ceiling. A shared
240,000-token Main Text KV pool serves admitted requests; either request may use the full capacity
when running alone, and two requests run concurrently when their complete reservations fit.

With `C=2` and two extra Device checkpoint slots, the process owns two active StateImage guarantees
plus a global pool of two Device-resident checkpoints. Eight pinned Host State slots and 8 GiB of
pinned Host KV retain inactive continuations under Device pressure. Active request capacity is two.

Other artifacts use the same command shape with their own path. For 35B-A3B text-only DFlash,
replace the MTP selection with `--spec dflash --draft-tokens 7 --lm-head-draft`; DFlash cannot be
combined with `--vision`.

When `--model-id` is omitted, the server advertises and accepts the loaded container's exact
`identity.model_id`. An explicit `--model-id` remains a public HTTP alias override and does not
select or alter the artifact.

Vision is disabled by default: its weights and Vision-specific unified-workspace extent are not
allocated, and media requests and token-count requests fail with HTTP 400 `vision_disabled`. Add
`--vision` when the server must accept image or video input. Speculative residency is likewise
frozen by `--spec mtp|dflash` and `--draft-tokens`; omitting `--spec` loads neither backend.
`--lm-head-draft` additionally loads the optimized proposal head. DFlash is 35B-A3B text-only and
cannot be combined with `--vision`. A later request cannot enable a capability omitted at startup.

## Endpoints

| Method and path | Behavior |
|---|---|
| `GET /health` | process health |
| `GET /v1/ninfer/status` | exact build/deployment identity, resolved runtime settings, activity, cache state, and MTP totals |
| `POST /v1/ninfer/checkpoints` | transactionally checkpoint the stored Responses session named by the exact JSON body |
| `GET /v1/ninfer/checkpoints/status` | authenticated durable-checkpoint compatibility and generation status; session credential in `X-NInfer-Session` |
| `DELETE /v1/ninfer/checkpoints` | remove the session named by `X-NInfer-Session` |
| `GET /v1/models` | configured OpenAI model alias |
| `GET /v1/models/{id}` | lookup of the configured alias |
| `POST /v1/chat/completions` | OpenAI-style chat generation |
| `POST /v1/responses` | OpenAI Responses Core generation, state, typed Items, and SSE |
| `POST /v1/responses/input_tokens` | Responses prompt-token count without generation |
| `GET /v1/responses/{id}` | retrieve a locally stored terminal Response |
| `DELETE /v1/responses/{id}` | delete a locally stored Response |
| `GET /v1/responses/{id}/input_items` | list that Response's normalized input Items |
| `POST /v1/messages` | Anthropic-style message generation |
| `POST /v1/messages/count_tokens` | checkpoint-native expanded input-token count |

## OpenAI Chat Completions

```bash
curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "qwen3.8-27b",
    "messages": [
      {"role": "system", "content": "Answer concisely."},
      {"role": "user", "content": "What is speculative decoding?"}
    ],
    "max_tokens": 128
  }'
```

The endpoint supports:

- `system`, `developer`, `user`, `assistant`, and `tool` history;
- string content and ordered text, `image_url`, and `video_url` parts; tool messages accept text
  parts only;
- `max_completion_tokens` and the legacy `max_tokens` spelling;
- `temperature`, `top_p`, `top_k`, presence/frequency penalties, and a nonnegative `seed`;
- one stop string or an array of stop strings;
- non-streaming responses and server-sent event streams;
- `stream_options.include_usage`;
- function tools, tool choices, assistant tool-call history, and tool-result messages;
- the top-level `reasoning_effort` field;
- the `enable_thinking` extension;
- `chat_template_kwargs.preserve_thinking` and the top-level `preserve_thinking` alias.

Tool-result `content` accepts either a string or an array of `text` content parts. Tool-result
media parts are rejected. Structured-decoding extensions—including `grammar`,
`structured_outputs`, JSON-schema/regex/EBNF fields, and `guided_*` variants—are rejected rather
than silently ignored; only `response_format: {"type":"text"}` is accepted.

The request `model` must equal the public model ID: the artifact `identity.model_id` by default, or
the explicit `--model-id` override. Reasoning is returned separately as `reasoning_content`; answer
text remains in `content`.

Across Chat Completions, Responses, and Anthropic Messages, an explicit top-level tool-parameter
type controls conversion of Qwen's untyped parameter text. String-admitting values remain strings;
other explicitly typed values are decoded as JSON without coercion. NInfer does not validate
generated arguments against the full JSON Schema.

Message roles retain their input order through schema translation. The Qwen family frontend maps
both `system` and `developer` to system-class ChatML blocks at their original positions; it does not
move later instructions to the beginning of the conversation. A leading instruction keeps the
artifact template's existing tool/reasoning-instruction composition.

At startup, NInfer resolves prompt capabilities from the exact `frontend/chat_template.jinja`
resource embedded in the loaded artifact. It does not infer them from the request's `model` field,
the artifact identity, or a target profile. A recognized effort-capable template exposes `low`,
`medium`, and `xhigh`; omitting effort uses that template's declared default. An explicit effort
not exposed by the loaded template returns HTTP 400 with code
`reasoning_effort_not_supported` before prompt preparation.

`--default-thinking-budget N` sets a positive process default for requests whose final resolved
prompt semantics enable thinking. It does not add or reinterpret an HTTP request field: the
existing `reasoning_effort` and `enable_thinking` inputs still decide whether thinking is enabled,
and a request resolved to non-thinking receives no cap. `--no-thinking` may coexist with this
option because a protocol request can explicitly enable thinking. In this phase, Anthropic's
existing `thinking.budget_tokens` member does not override the process default.

Add `--default-thinking-budget 512` to the startup command to cap model-origin thinking at 512
tokens for every thinking-enabled request.

At the cap boundary, Engine first honors a natural `</think>`, stop condition, cancellation, or
total output/context limit. If thinking remains open, it commits Qwen's canonical early-close
guidance and close marker to the same model sequence without sampling, streams the guidance as a
reasoning delta, and continues normal content or tool-call generation. Inserted tokens count in
completion usage and the request's `max_tokens`/`max_output_tokens` budget. If the effective output
capacity extends past the cap but cannot fit the complete tokenizer-derived control suffix plus one
post-close model token, preparation is rejected with HTTP 400 code
`thinking_budget_capacity_insufficient` rather than partially inserting control. The server does
not promise that the model will emit nonempty content or a tool call after the marker.

For Chat Completions, `reasoning_effort: "none"` disables thinking. `low`, `medium`, and `xhigh`
select the corresponding template effort when available. The other OpenAI protocol values
`minimal`, `high`, and `max` are parsed but rejected when the loaded template does not expose them.
`enable_thinking` controls the same new-turn thinking switch; a contradictory combination with
`reasoning_effort` returns `conflicting_template_option`.

`preserve_thinking` controls whether reasoning from closed assistant turns remains in later
prompts. It defaults to the server setting, which is off unless `--preserve-thinking` is used. If
both OpenAI spellings are present they must carry the same boolean value. Unknown non-null
`chat_template_kwargs` are rejected.

Streaming begins with an assistant-role chunk, sends separate reasoning and content deltas, then a
finish-reason chunk and `[DONE]`. When `stream_options.include_usage` is true, a final empty
`choices` chunk contains completed usage.

### Multimodal request

Start the server with `--vision` before sending media:

```bash
curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "qwen3.8-27b",
    "messages": [{
      "role": "user",
      "content": [
        {"type": "image_url", "image_url": {"url": "https://example.com/image.png"}},
        {"type": "text", "text": "Describe this image."}
      ]
    }],
    "max_tokens": 128
  }'
```

OpenAI image and video sources may be HTTP(S) URLs or base64 data URLs.

Text and media requests use one complete-prompt context contract. After chat-template rendering and
media-token expansion, the result must fit Engine `--max-context`. The current Vision runtime also
has a 32,768 merged-token envelope (131,072 raw patches); the effective Vision limit is therefore
`min(--max-context, 32768)`. There is no fixed image/video item-count limit: item count is admitted
through aggregate source-byte, decoded-pixel, raw-patch, Vision-token, and live-memory budgets.

Media cache misses run as independent decode → resize → BF16-pack tasks on a bounded host worker
pool. Prepared payloads are keyed by SHA-256 of the acquired bytes plus modality, so repeated media
in later requests reuses the exact immutable BF16 patch input; concurrent identical misses use one
single-flight build. `--media-cache-mib` bounds LRU-retained payloads, while
`--media-live-mib` bounds every cache-, request-, or runtime-referenced payload. Cache eviction does
not invalidate a request reference, and live bytes are returned only when the final reference is
released. A request-level preparation gate derived from the live limit prevents concurrent partial
builds from deadlocking the memory account.

An expanded prompt beyond `--max-context` returns HTTP 400 `context_length_exceeded`, including
the prepared token count and configured context ceiling. A media preprocessing resource rejection
returns HTTP 400 `media_budget_exceeded`. HTTP 413 `request_too_large` is reserved for a raw request
body that exceeds `--max-request-mib` before JSON parsing; it is not used for model-context or media
resource errors.

## OpenAI Responses Core

NInfer implements the typed-Item and semantic-event core of the OpenAI
[Responses API](https://developers.openai.com/api/reference/resources/responses/overview). All
registered artifact identities use this same adapter and Engine route. It is intentionally not
advertised as full parity with OpenAI-hosted tools, durable cloud storage, background jobs,
Conversations, or compaction.

### Create a Response

```bash
curl http://127.0.0.1:8080/v1/responses \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "qwen3.8-27b",
    "instructions": "Answer concisely.",
    "input": "What is speculative decoding?",
    "max_output_tokens": 128,
    "store": true
  }'
```

The same endpoint works with OpenAI SDKs by replacing their base URL:

```python
from openai import OpenAI

client = OpenAI(base_url="http://127.0.0.1:8080/v1", api_key="local-secret")
response = client.responses.create(
    model="qwen3.8-27b",
    instructions="Answer concisely.",
    input="What is speculative decoding?",
    max_output_tokens=128,
)
print(response.output_text)  # SDK helper derived from response.output
```

`output_text` is an SDK convenience property. It is not emitted as a top-level wire field; the
wire response contains typed `output` Items.

### Create request fields

| Field | NInfer Responses Core contract |
|---|---|
| `model` | required non-empty string; must equal the artifact-derived public model ID or explicit `--model-id` override |
| `input` | required string or non-empty typed Item array |
| `instructions` | optional string, inserted before the reconstructed conversation for this request only |
| `previous_response_id` | optional ID of a retained local Response |
| `ninfer_session` | optional 64-character lowercase SHA-256; authenticated Engine lineage and continuation namespace |
| `ninfer_request_id` | optional 64-character lowercase SHA-256 used only for request-log correlation |
| `max_output_tokens` | integer at least `16`; default is `--default-max-tokens` |
| `stream` | boolean; `true` selects Responses SSE rather than a JSON body |
| `store` | boolean, default `true`; controls local retrieval and continuation state |
| `temperature` | finite number in `[0,2]` |
| `top_p` | finite number in `[0,1]` |
| `metadata` | at most 16 string pairs; keys at most 64 characters and values at most 512 |
| `reasoning.effort` | `none` disables thinking; `low`, `medium`, or `xhigh` selects an effort exposed by the loaded chat template; `minimal`, `high`, and `max` return `reasoning_effort_not_supported` for the registered templates |
| `chat_template_kwargs.preserve_thinking` | optional boolean controlling whether closed-turn reasoning remains in reconstructed prompts |
| `preserve_thinking` | top-level alias for the same option; conflicting values are rejected |
| `text.format` | omitted or `{"type":"text"}` only |
| `tools` | flat Responses `function` or `custom` definitions; see below |
| `tool_choice` | `auto` or `none` |
| `parallel_tool_calls` | omitted or `true` |
| `truncation` | omitted or `disabled`; overlong input fails instead of silently dropping Items |
| `top_logprobs` | omitted or `0` |
| `service_tier` | omitted, `auto`, or `default`; the response reports `default` |
| `background` | omitted or `false` |
| `include` | omitted or an empty array |
| `stream_options` | omitted or `{"include_obfuscation":false}` |

Unknown top-level fields fail with `unknown_parameter`. Recognized but unsupported features fail
with a field-specific 400 error instead of being silently ignored.

### Input Item contract

String `input` is normalized to one user `message` with an `input_text` part. Array input accepts:

| Item | Supported form |
|---|---|
| `message` | roles `user`, `assistant`, `system`, and `developer`; string content or typed content array |
| `input_text` | message content part containing string `text` |
| `output_text` | assistant-message replay part containing string `text` |
| `input_image` | user-message part with HTTP(S) or data-URI `image_url`; detail omitted or `auto`; requires server `--vision` |
| `input_video` | NInfer extension with HTTP(S) or data-URI `video_url`; requires server `--vision` |
| `reasoning` | raw replay Item with an empty `summary` and `reasoning_text` content parts |
| `function_call` | completed assistant call with optional `id`, and required `call_id`, `name`, and JSON-object string `arguments` |
| `function_call_output` | completed tool result with required `call_id` and string `output` |
| `custom_tool_call` | completed assistant call with optional `id`, and required `call_id`, `name`, and raw string `input` |
| `custom_tool_call_output` | completed custom-tool result with required `call_id` and string `output` |

Adjacent function/custom-call Items are grouped into one assistant history turn. A reasoning Item
attaches to the following assistant message or tool call. Input Item IDs are preserved when
supplied and generated otherwise; duplicate IDs fail.

System and developer message Items retain their positions in the input array. Top-level
`instructions` is represented as a leading developer turn for the current request; target-specific
role lowering occurs only in the Qwen family frontend.

`input_file`, `input_audio`, image `file_id`, non-`auto` image detail, reasoning summaries or
encrypted reasoning, message `phase`, and other Item/content types are not supported. HTTP media
URLs stored in a response chain are fetched again when that chain is continued; use data URIs when
the historical media bytes must be immutable.

### Function and custom tools

Responses function definitions are flat rather than Chat Completions' nested `function` object:

```json
{
  "type": "function",
  "name": "get_weather",
  "description": "Get current weather",
  "parameters": {
    "type": "object",
    "properties": {"city": {"type": "string"}},
    "required": ["city"]
  },
  "strict": false
}
```

NInfer renders these definitions in the Qwen prompt and parses model output into separate
`function_call` output Items. Each output has a protocol Item `id` (`fc_...`) and a distinct
`call_id` (`call_...`). The client executes the function and sends a `function_call_output` Item in
a later request.

Responses custom tools accept the OpenAI free-form text and grammar format shapes:

```json
{
  "type": "custom",
  "name": "eval",
  "description": "Evaluate one code cell",
  "format": {"type": "grammar", "syntax": "lark", "definition": "start: /.+/"}
}
```

`format` may be omitted or use `{"type":"text"}`. Grammar formats require a non-empty
`definition` and `syntax` of `lark` or `regex`; malformed and unknown formats fail explicitly.
NInfer maps a custom definition to one string parameter named `input` in the Qwen tool prompt and
maps the generated value back to `custom_tool_call.input` without JSON interpretation. Custom
output Items use `ctc_...` Item IDs and distinct `call_...` pairing IDs. NInfer does not execute
tools or enforce JSON Schema/grammar through constrained decoding. Grammar is untrusted prompt
guidance only: NInfer echoes it for wire continuity, and clients must validate generated input
before execution. Therefore `strict:true`,
`tool_choice:required`, named tool choice, hosted tools, MCP tools, and unsupported custom-tool
options are rejected.

### Response object and usage

A terminal wire response has `object: "response"`, one of `completed`, `incomplete`, or
`cancelled` in `status`, and a typed `output` array. NInfer may emit:

- a `reasoning` Item containing raw `reasoning_text` and an empty summary;
- an assistant `message` containing an `output_text` part;
- one or more `function_call` or `custom_tool_call` Items.

Ordinary model/string stops produce `completed`. Output-token or context-capacity exhaustion
produces `incomplete` with `incomplete_details.reason: "max_output_tokens"`. Errors accepted after
an SSE response has started produce `response.failed`; validation and preparation errors remain
normal HTTP error responses.

Usage is checkpoint-native:

```json
{
  "input_tokens": 42,
  "input_tokens_details": {"cached_tokens": 17},
  "output_tokens": 12,
  "output_tokens_details": {"reasoning_tokens": 5},
  "total_tokens": 54
}
```

`input_tokens` includes the chat template and expanded media tokens. `cached_tokens` is the exact
checkpoint-proven prompt prefix reused by Engine. `output_tokens` is the count of accepted generated token
IDs, including a withheld stop token when applicable. `reasoning_tokens` is counted in the Qwen
output decoder while accepted tokens are still in the reasoning channel; it is not estimated by
re-tokenizing decoded text.

### Responses streaming

Set `stream:true` for semantic Server-Sent Events. Every frame uses both the SSE event name and a
matching JSON `type`, and every JSON event has a monotonically increasing `sequence_number`:

```text
event: response.output_text.delta
data: {"type":"response.output_text.delta","sequence_number":7,...}

```

The normal lifecycle is:

1. `response.created`, then `response.in_progress`;
2. `response.output_item.added` and `response.content_part.added`;
3. zero or more `response.reasoning_text.delta` or `response.output_text.delta` events;
4. matching `*.done`, `response.content_part.done`, and `response.output_item.done` events;
5. exactly one `response.completed`, `response.incomplete`, `response.cancelled`, or
   `response.failed` terminal event.

Function arguments use `response.function_call_arguments.delta` and `.done`. IDs, output indices,
and content indices remain stable, and concatenated deltas equal the terminal Item. Custom input
uses `response.custom_tool_call_input.delta` and `.done`, with the same stable Item and call IDs.
Responses SSE does not emit the Chat Completions `[DONE]` sentinel. With tools enabled, ordinary
answer text still streams immediately; only an ambiguous `<tool_call>` suffix or the structured
tool region is held. Malformed tool markup is flushed back as ordinary text without losing bytes.

### Local response state and resources

`store` defaults to `true`. Stored Responses are bounded by an LRU store. Without
`--session-checkpoint-dir`, they live only in this server process and are lost on restart. The
optional checkpoint path described below is local NInfer acceleration state, not OpenAI durable
cloud retention.

`previous_response_id` reconstructs the complete stored input/output Item history before the new
input. The current `instructions` value is placed first but is not saved into the continuation
context, matching the Responses rule that previous top-level instructions do not carry forward.
Function definitions are request configuration rather than conversation Items and must be sent
again on tool-result turns. The reconstructed prompt follows the ordinary Engine path, so compatible
checkpoint reuse applies naturally.
When a stored Response carries `ninfer_session`, every `previous_response_id` continuation must
send the same digest. Retrieve, delete, input-Item, and cancel requests carry that digest in the
`X-NInfer-Session` header because those routes have no request body. A different, duplicated, malformed,
or omitted session cannot read, delete, or continue that session's DAG. Failed
`previous_response_id` continuation returns 404 `previous_response_not_found`; retrieval, delete,
input-Item, and cancel routes retain 404 `response_not_found`. The header is accepted only when API
authentication is configured and is included in the CORS allowlist. Every fork from the same parent
and session selects the same `http:<digest>` Engine lineage. Parent deletion does not invalidate
already stored descendants. Changing `ninfer_request_id` never changes lineage selection.

Treat `ninfer_session` as a credential, not a label. Compute the 64-character lowercase digest from a
raw session identifier with at least 128 bits of CSPRNG entropy (or an equivalent keyed construction),
never from a user name or other guessable value, and never place the digest in a URL or access log.
Callers sharing one API key and process still share the bounded global Response-store capacity and can
evict one another's least-recently-used records; the session boundary protects confidentiality and
integrity, not availability. Use separate server instances/API keys when availability isolation is
required.


A stored Response also retains its resolved `preserve_thinking` value. A child which omits the
field inherits the parent value. An explicit different value creates a new semantic branch; prompt
rendering and identity still determine reuse. Changing the boolean alone never invalidates an exact
checkpoint already proved compatible by the model runtime.

For Engine-local reuse, a stored root Response receives one bounded session key derived from its
response ID, and every `previous_response_id` child inherits that key. `store:false` roots remain
anonymous; a `store:false` child may read its inherited session checkpoint but does not replace the
stored chain's latest endpoint. Response-store eviction or deletion removes the HTTP object, not an
independently retained Engine checkpoint; the latter remains bounded by the Engine's own retention
and pressure policy. No session key or cache marker is added to the HTTP schema.

Resource behavior:

| Endpoint | Contract |
|---|---|
| `GET /v1/responses/{id}` | returns the stored terminal object, or 404 `response_not_found`; scoped records require matching `X-NInfer-Session` |
| `DELETE /v1/responses/{id}` | requires matching `X-NInfer-Session` for scoped records, removes public retrieval, and returns `response.deleted`; descendant contexts already retained by other Responses remain usable |
| `GET /v1/responses/{id}/input_items` | requires matching `X-NInfer-Session` for scoped records and returns normalized Items supplied to that request; supports `after`, `limit` `1..100` (default `20`), and `order` `asc|desc` (default `desc`) |
| `POST /v1/responses/{id}/cancel` | requires matching `X-NInfer-Session` for scoped records, then explicitly fails because background execution is unsupported |
| `POST /v1/responses/compact` | explicitly fails with `compaction_not_supported` |

`store:false` Responses cannot be retrieved or used as `previous_response_id`. LRU eviction and
explicit deletion also make an ID unavailable. A single Response larger than the configured store
capacity fails with `response_store_capacity_exceeded` rather than silently pretending it was
stored.

### Durable session checkpoints

`--session-checkpoint-dir DIR` enables authenticated, local persistence for stored Responses that
carry a lowercase SHA-256 `ninfer_session`. Startup then also requires `--api-key`, exact
`--binary-sha256`, `--artifact-sha256`, and `--config-sha256` declarations, prefix reuse, at least
one Host StateImage slot, and nonzero Host KV capacity. The checkpoint fingerprint additionally
binds the loaded target/model/weights identity and relevant Engine/cache configuration. An
incompatible fingerprint is a cache miss; it is never imported under a different executable,
artifact, or configuration.

Restore I/O is platform-native and fail-closed. Native Windows uses DirectStorage 1.3; Linux and
the primary WSL2/container lane use `io_uring` with `O_DIRECT` on a supported local filesystem.
Server startup rejects a configured checkpoint directory when that backend's exact capability
probe fails. Status reports the selected `read_backend`. DirectStorage does not read SMB/NAS, S3,
VHDX, or WSL paths: any future remote replication must first materialize and verify an immutable
generation on eligible local storage. Save publication remains manifest-last durable filesystem
I/O because DirectStorage queues are read-only.

The routes require the server bearer API key. Save takes the digest only in one exact JSON body;
malformed JSON, a missing or non-string digest, a non-lowercase/non-SHA-256 digest, or any extra
field is HTTP 400:

```json
{"session_sha256":"<64 lowercase hex>"}
```

| Endpoint | Contract |
|---|---|
| `POST /v1/ninfer/checkpoints` | save the complete stored lineage and the exact catalogued Engine endpoint whose checkpoint tag is that lineage's latest Response ID; return HTTP 409 when no complete checkpointable endpoint exists |
| `GET /v1/ninfer/checkpoints/status` | read the digest from the required `X-NInfer-Session` header and report `available`, `missing`, `incompatible`, `corrupt`, or transient `unavailable` state |
| `DELETE /v1/ninfer/checkpoints` | read the digest from the required `X-NInfer-Session` header, atomically rename the complete session to an internal tombstone, and retire it; return `deleted`/200, genuine `missing`/404, or `conflict`/409 when a live reader or filesystem refusal prevents the rename; physical cleanup is best-effort and retried by garbage collection |

Success and status bodies never echo the session digest or include prompt, response, tool, or
reasoning content.

Each save writes a new generation, flushes payloads and checksums, writes the manifest last, then
atomically replaces `current`. Publication requires an Engine export that can restore at least 95%
of its compatible frontier, and its reported payload bytes must exactly match the generated Engine
files. An interrupted save leaves the previous generation usable. Restore verifies every file and
the runtime fingerprint before exposing state. On the first authenticated `previous_response_id`
lookup missing from the in-memory store, NInfer lazily imports the matching Engine continuation.
The imported frontier and payload summary must equal the manifest before Engine catalog ownership
and the typed Responses snapshot publish in one transaction. Any failure returns the ordinary
missing-response behavior so the client can perform its explicit full replay. Verified structural
or checksum corruption is quarantined and ignored. A transient missing/open/read filesystem failure
reports `unavailable` without changing the generation or `current` pointer, so a later request can
retry the same checkpoint.

A completed stored turn at or above `--session-checkpoint-min-tokens` is enqueued to one
bounded background checkpoint worker; the default threshold is `32768`. The request publishes its
terminal HTTP/SSE response without waiting for checkpoint I/O, repeated saves for the same session
coalesce, and a full queue drops only that automatic acceleration attempt. Graceful server shutdown
drains the worker and then attempts every live session. Explicit `POST` remains the synchronous
crash-test boundary: do not kill the process until it returns.
`--session-checkpoint-staging-mib` bounds transfer/codec staging.
`--session-checkpoint-quota-mib` is a store-wide cap over all retained current and stale
generations, including deferred tombstones. Before publishing a new `current`, admission cleans
tombstones and abandoned staging, then reclaims the oldest inactive stale generations and inactive
current sessions, with path order breaking equal timestamps. Reclaimed paths are atomically renamed
to internal tombstones before physical cleanup, so no current pointer can dangle. The generation
being published, its session, and every active restore reader are never eviction candidates. A
cleanup failure or insufficient reclaimable space refuses the save before its prior `current`
changes; retained tombstones remain quota-accounted and are retried at startup and by garbage
collection.

### Responses input token count

`POST /v1/responses/input_tokens` accepts `model`, `input`, the two authenticated NInfer identity
fields, and the thinking-history option, performs the same typed Item, template, and media expansion,
and does not run generation:

```bash
curl http://127.0.0.1:8080/v1/responses/input_tokens \
  -H 'Content-Type: application/json' \
  -d '{"model":"qwen3.8-27b","input":"Count this prompt."}'
```

```json
{"object":"response.input_tokens","input_tokens":11}
```

Unsupported Create fields include Conversations, prompt templates, context management, hosted
moderation, prompt-cache controls, safety/user identifiers, Structured Outputs/JSON mode,
non-empty `include`, background execution, compaction, files/audio, and OpenAI-hosted/MCP tools.
These are compatibility boundaries, not silently accepted placeholders.

## Anthropic Messages

```bash
curl http://127.0.0.1:8080/v1/messages \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "qwen3.8-27b",
    "max_tokens": 128,
    "messages": [
      {"role": "user", "content": "Explain prefix reuse in one sentence."}
    ]
  }'
```

The endpoint supports top-level system text, ordered mid-conversation system messages,
user/assistant history, text and image blocks, thinking blocks, tool-use history, tool results,
client-defined tools, non-streaming responses, and Anthropic SSE events.
Mid-conversation system messages remain at their `messages` array position and are not merged into
the top-level system instruction. A system section must follow a user/tool-result message and be
final or immediately precede an assistant message; it cannot interrupt a tool-use/tool-result pair.
Consecutive system messages remain separate ordered turns.

Anthropic ephemeral `cache_control` on top-level system text blocks and client tool definitions
marks a shared stable-prefix boundary. Because the Qwen prompt renders tools before the leading
system instruction, NInfer retains the last marked tool boundary unless a later marked system
boundary exists; when several system blocks are marked, it retains the last one. The boundary is a
retention hint rather than a forced hit: reuse still requires exact rendered-token compatibility.
Ephemeral `cache_control` on the final content block of a user or assistant message marks the
normalized message boundary as a private long anchor; a non-final message-block breakpoint is
rejected because it cannot be represented as an exact Qwen message frontier.

`thinking.type: "disabled"` disables thinking; other supported values enable it.
The independent top-level `preserve_thinking` boolean controls closed-turn history and otherwise
uses the server default.

Anthropic `output_config.effort` accepts the protocol values `low`, `medium`, `high`, `xhigh`, and
`max`. The value is then checked against the loaded chat template in the same way as the OpenAI
endpoints; the registered effort-capable template exposes `low`, `medium`, and `xhigh`. Combining
an effort with `thinking.type: "disabled"` is rejected as contradictory.

Anthropic's `model` field is treated as a response label and does not select the loaded artifact.

`POST /v1/messages/count_tokens` uses the artifact's tokenizer, chat template, and media expansion
without running GPU generation:

```bash
curl http://127.0.0.1:8080/v1/messages/count_tokens \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "qwen3.8-27b",
    "messages": [{"role": "user", "content": "Count this prompt."}]
  }'
```

## Authentication and CORS

Pass `--api-key VALUE` to require the same value as an OpenAI bearer token or Anthropic
`x-api-key` header. `GET /health` and CORS preflight requests remain unauthenticated.

OpenAI Chat Completions, OpenAI Responses, and Anthropic Messages accept optional top-level
`ninfer_session` and `ninfer_request_id` fields for trusted local-agent clients. Each value must be
a caller-computed 64-character lowercase SHA-256 digest; raw session or request identifiers must
not be sent. A valid `ninfer_session` selects the private `http:<digest>` cache lineage with
live-session retention, while `ninfer_request_id` provides request-log correlation only. Either
field is rejected with HTTP 401 unless the server was started with `--api-key`; normal endpoint
authentication then ensures only authenticated callers can select those cache and log identities.

`GET /v1/ninfer/status` is always authenticated: it returns HTTP 401 when the server has no
configured API key, and the ordinary API-key pre-route rejects a missing or invalid credential. Its
`ninfer_server_status` schema-v1 body has stable `identity`, `runtime`, `scheduler`, `cache`, and
`mtp` groups. Those groups contain declared release/deployment/model digests, resolved static
runtime settings, published scheduler counters, bounded cache gauges, and aggregate MTP totals. The
route performs no filesystem hashing, device query, prompt lookup, or response-store traversal and
contains no paths, credentials, prompts, generated text, GPU UUID, or process arguments.
`GET /health` remains unauthenticated.

```bash
curl http://127.0.0.1:8080/v1/models \
  -H 'Authorization: Bearer local-secret'
```

`--cors` adds permissive browser CORS headers. It is disabled by default.

## Server options

The table lists executable defaults. The startup example selects a long-context FP8/MTP3 profile.

| Option | Meaning | Default |
|---|---|---:|
| `--host H` | listen address | `127.0.0.1` |
| `--port N` | listen port | `8080` |
| `--api-key KEY` | required bearer or `x-api-key` value | unset |
| `--model-id ID` | override the public OpenAI model alias | artifact `identity.model_id` |
| `--binary-sha256 SHA` | lifecycle-measured executable SHA-256 declaration | unset |
| `--artifact-sha256 SHA` | lifecycle-measured model artifact SHA-256 declaration | unset |
| `--config-sha256 SHA` | lifecycle-measured canonical configuration SHA-256 declaration | unset |
| `--deployment-profile NAME` | generic deployment identity matching `[A-Za-z0-9._-]{1,64}` | unset |
| `--max-context N` | logical context ceiling of each sequence | `8192` |
| `--kv-capacity N\|auto` | explicit shared Main Text KV capacity, or maximize it from remaining GPU memory; omitted means `--max-context` | `8192` |
| `--max-concurrency N` | maximum admitted requests; valid range `1..8` | `1` |
| `--max-pending-requests N` | additional requests allowed to wait for admission | `16` |
| `--pending-timeout-ms N` | maximum preparation-plus-admission wait | `30000` |
| `--prefill-chunk N` | text-prefill chunk | `1024` |
| `--log-stats-interval-ms N` | aggregate throughput report interval; `0` disables it | `5000` |
| `--device N` | CUDA device index | `0` |
| `--context-cost-presets FILE` | optional runtime context-cost preset registry | generic + compiled defaults |
| `--max-request-mib N` | body-size limit before JSON parsing | `384` |
| `--media-cache-mib N` | LRU-retained prepared BF16 media payloads; `0` disables retention | `1024` |
| `--media-live-mib N` | all live prepared BF16 media payloads | `2048` |
| `--media-preprocess-threads N` | bounded media preprocessing workers; `0` selects at most 16 from host concurrency | `0` |
| `--request-log-jsonl FILE` | append full-precision server/request records | disabled |
| `--response-store-max-records N` | maximum locally retained Responses objects | `1024` |
| `--response-store-max-mib N` | total local Response envelope/Item/context budget | `256` |
| `--session-checkpoint-dir DIR` | enable authenticated durable session generations at this root | disabled |
| `--session-checkpoint-quota-mib N` | total retained durable-checkpoint budget | `65536` |
| `--session-checkpoint-staging-mib N` | bounded checkpoint codec and transfer staging | `256` |
| `--session-checkpoint-min-tokens N` | completed-turn frontier that triggers an automatic save | `32768` |
| `--kv-dtype bf16\|int8\|fp8` | KV-cache storage | `bf16` |
| `--spec mtp\|dflash` | speculative backend | off |
| `--draft-tokens N` | MTP `1..5`; DFlash `1..15` | unset |
| `--lm-head-draft` | optimized proposal head | off |
| `--default-max-tokens N` | output limit when omitted by a request | `8192` |
| `--default-thinking-budget N` | positive thinking cap inherited by thinking-enabled requests | unset |
| `--vision` | enable media input and load Vision GPU allocations | off |
| `--no-cuda-graph` | disable CUDA Graph decode | graphs on |
| `--no-prefix-reuse` | disable compatible-prefix caching | prefix reuse on |
| `--device-state-slots N` | extra Device checkpoint StateImages beyond the active-lane guarantee | `max-concurrency` |
| `--host-state-slots N` | pinned Host StateImage capacity | `8` |
| `--host-kv-mib N` | shared pinned Host Main/Backend KV byte capacity in MiB | `8192` |
| `--max-private-continuations N` | private continuation descriptor capacity | `2 * max-concurrency` |
| `--max-shared-prefixes N` | shared stable-prefix descriptor capacity | `max-concurrency` |
| `--max-long-anchors-per-continuation N` | private long-anchor limit per continuation | `2` |
| `--max-cache-markers-per-request N` | caller marker input-complexity bound | `4` |
| `--no-thinking` | disable thinking by default | thinking on |
| `--preserve-thinking` | preserve closed-turn assistant reasoning by default | off |
| `--cors` | permissive browser CORS headers | off |
| `--temperature F` | process-level temperature override | unset |
| `--top-p F` | process-level top-p override | unset |
| `--top-k N` | process-level top-k override | unset |
| `--min-p F` | process-level min-p override | unset |
| `--presence-penalty F` | process-level presence-penalty override | unset |
| `--frequency-penalty F` | process-level frequency-penalty override | unset |
| `--seed N` | fixed seed when a request omits one | fresh random seed per request |
| `--greedy` | force exact argmax for all requests | off |

Context-cost coefficients resolve once at startup from generic defaults, matching compiled values,
and optional transfer or artifact-prefill entries from `--context-cost-presets FILE`. A malformed
file aborts startup; the startup console line and JSONL record identify the selected source.

Engine selects sampling defaults from the loaded model and the request's resolved thinking mode.
Qwen3.6-27B and Qwen3.8-27B use `1.0/0.95/20/0/0` for
temperature/top-p/top-k/min-p/presence penalty in thinking mode and `0.7/0.80/20/0/1.5` in
non-thinking mode. Qwen3.6-35B-A3B differs only in its thinking presence penalty, which is `1.5`.
Frequency penalty is `0` for all registered presets. Process flags override registered values,
request fields override process flags, and `--greedy` finally forces temperature `0`.

For `C=--max-concurrency` and `H=--device-state-slots`, total Device StateImage capacity is `C+H`:
`C` slots guarantee active requests and `H` is a global checkpoint pool. Host State and Host KV are
independent startup-fixed pinned-memory capacities; Host KV is shared by Main and the selected
Backend pool and is consumed in physical page extents. `--no-prefix-reuse` selects root-only Engine
mode and cannot be combined with any of the seven explicit context-cache capacity flags, including
zero-valued flags.

Run `./build/apps/ninfer-serve --help` for the exact option contract.

## Reproducible container lifecycle

`tools/lifecycle/ninfer_container.py` builds and operates one isolated Docker candidate from a
Linux or WSL host. It never changes a shared route or stops another container. Candidate ports bind
to loopback by default. Every lifecycle configuration requires `api_key_file` because startup and
status verification use the authenticated status contract. An explicit `bind_host` of `0.0.0.0`
accepts that same secret-backed configuration; any other bind address is rejected. `start` refuses
an existing container name or listening port, and `stop` refuses a container without the lifecycle
ownership label.

Build the runtime image from an exact clean commit with the upstream and patch-stack identities
embedded in both binaries. The lifecycle refuses any tracked or untracked source change, verifies
that the upstream commit is an ancestor, and gives Docker a Git archive of the measured release
head rather than the working tree. Only that verified archive can set `source_dirty` to `false`:

```bash
python3 tools/lifecycle/ninfer_container.py build \
  --image ninfer:canary \
  --upstream-base-sha 6e8b2e2ad5d53597c3ba8e7989f9546d40b921fc \
  --expect-patch-stack-sha "$(git rev-parse HEAD)" \
  --build-profile qwen38-5090-mainline
```

Runtime configuration is one JSON object. `args` contains individual server argv elements; the
lifecycle-owned model, network, identity, authentication, request-log, and checkpoint-path options
are rejected there to prevent ambiguous duplicates.

```json
{
  "image": "ninfer:canary",
  "bind_host": "127.0.0.1",
  "container": "ninfer-canary",
  "model_path": "/srv/ninfer/models/model.ninfer",
  "model_id": "registered-model",
  "deployment_profile": "rtx-5090-int8-mtp3",
  "port": 18088,
  "request_log_dir": "/srv/ninfer/logs/canary",
  "checkpoint_dir": "/srv/ninfer/checkpoints/canary",
  "api_key_file": "/run/secrets/ninfer_api_key",
  "restart_policy": "no",
  "args": [
    "--max-context", "131072",
    "--kv-capacity", "auto",
    "--kv-dtype", "int8",
    "--max-concurrency", "1",
    "--spec", "mtp",
    "--draft-tokens", "3",
    "--lm-head-draft",
    "--session-checkpoint-quota-mib", "65536",
    "--session-checkpoint-staging-mib", "2048"
  ]
}
```

All paths are absolute host paths visible to Docker. When `checkpoint_dir` is present, `start`
creates it with mode `0700`, mounts it at `/checkpoints`, and owns the corresponding
`--session-checkpoint-dir`; `args` cannot redirect durable state elsewhere. The API-key value is
read from a read-only secret mount inside the container; it is not placed in Docker argv, labels,
receipts, or the canonical configuration identity. `restart_policy` defaults to `no`; use
`unless-stopped` only for a promoted appliance that Docker must restart with its daemon. `start`
applies the declared policy, and `status` rejects policy drift. The canonical configuration SHA-256 covers model ID, deployment
profile, bind address and port, API-auth presence, request-log presence, checkpoint-mount presence,
restart policy, and ordered server args. Binary and model bytes have separate SHA-256 identities.

```bash
python3 tools/lifecycle/ninfer_container.py preflight --config candidate.json
python3 tools/lifecycle/ninfer_container.py start --config candidate.json
python3 tools/lifecycle/ninfer_container.py health --config candidate.json
python3 tools/lifecycle/ninfer_container.py status --config candidate.json

python3 tools/smoke/serve_contract.py \
  --base-url http://127.0.0.1:18088 --model registered-model \
  --api-key-file /run/secrets/ninfer_api_key
python3 tools/smoke/agent_protocol.py \
  --base-url http://127.0.0.1:18088 \
  --model registered-model \
  --api-key-file /run/secrets/ninfer_api_key

python3 tools/lifecycle/ninfer_container.py stop --config candidate.json
```

`preflight`, `start`, and `status` accept optional `--expect-image-id`,
`--expect-binary-sha256`, `--expect-model-artifact-sha256`, and
`--expect-config-sha256` pins. Receipts and status output contain identities and counters but omit
model/log paths, process argv, API-key values, prompts, and generated text.

## Structured request log

`--request-log-jsonl FILE` enables the machine-readable measurement log. The server opens `FILE`
in append mode and flushes every event, so successive model or MTP blocks may share one campaign
file. The parent directory must already exist. Failure to open the file aborts startup; the log path
is also rejected if it resolves to the model artifact.

Add `--request-log-jsonl profiles/bench/run/server.requests.jsonl` to the startup command to write
the log at that path.

Every line is one `ninfer_serve_request_log` schema-v17 JSON object. All events carry
`timestamp_unix_ms` and a process-unique `server_instance_id`; request IDs are monotonic only within
that server instance. Successful request-start records include request-scoped acquisition,
media-preprocessing wall/work, tokenizer, cache hit/miss/single-flight, and payload-size fields;
they do not infer request behavior from process-global counter deltas.

| Event | Contents |
|---|---|
| `server_start` | exact build/deployment/model identity and artifact, resolved Engine and context-cache capacities, registered thinking/non-thinking sampler defaults plus process overrides, thinking-history and thinking-budget defaults, Device arenas, the optional non-additive Vision layout inside the unified workspace, Host State/KV capacity and occupancy, KV sizing ledger, CUDA Graph observed/allowance bytes, CUDA/GPU environment, and redacted argv |
| `request_start` | protocol, optional client session/request SHA-256 correlation, resolved sampler and seed, thinking mode and optional budget, Responses semantic-change flag, output budget, stream/message/tool shape |
| `request_rejected` | parsed request shape, optional client session/request SHA-256 correlation, media-item count, `phase: "prepare"`, and the exact HTTP status/type/code/parameter/message for a synchronous preparation rejection |
| `request_done` | optional client session/request SHA-256 correlation, finish reason, prompt/completion/cache/computed-prefill tokens, prefix reuse path, request-owned materialization cost/search/bound diagnostics, thinking-budget application counters, unrounded request-stage seconds, per-request Engine Host exposure, and complete speculative-decoding counters |
| `request_error` | optional client session/request SHA-256 correlation, the resolved request configuration, and generation error message |
| `throughput` | interval token/decode/context-cache pressure counter deltas, authoritative worker Host-work deltas, current scheduler/resource gauges, latest materialization prediction, complete MTP counters, and decode-round batch statistics |

`request_done.materialization` is the immutable decision committed for that request. It reports predicted immediate,
future-loss and total nanoseconds; evaluated targets and projection work; planning/search nanoseconds; stop reason;
model-optimal and budget-exhausted flags; the best remaining lower bound and absolute/relative gap; selected degradation
units; and whether the selected target was the maximal root fallback. Stop reasons are `no_pressure`, `model_optimal`,
`queue_exhausted`, `target_budget`, `expansion_capacity`, `time_budget`, and `value_of_next_expansion`.
`model_optimal` is relative to the configured machine/cache-value model, semantic target graph, and canonical stage
contract—not a claim that observed TTFT is globally optimal. Aborted planning attempts are not published.

`request_done.timings_seconds` contains `prepare`, `ttft`, `vision`, `prefill`, `decode`, and `total`
as full-precision JSON numbers. Its `speculative` object contains `backend`, `draft_window`, `rounds`,
`drafted_tokens`, `accepted_tokens`, `fallback_steps`, and `accepted_per_position`. Rates can be
derived downstream from raw token counts and seconds instead of rounded stderr strings.

For `server_start.memory`, `workspace.capacity_bytes` is the only physical workspace allocation.
When Vision is enabled, `vision_workspace` reports the aggregate prompt and maximum-item token
bounds plus encode peak and handoff layout/usage within that same allocation; these bytes must not
be added to `workspace.capacity_bytes`. The field is `null` when Vision is disabled.

`request_done.engine_timing` separates FIFO `queue_wait_seconds`, blocking
`device_wait_exposed_seconds`, and five mutually exclusive Host-active exposure phases under
`host_exposed_seconds`: `engine_boundary`, `program_submit`, `program_post`,
`engine_commit_output`, and `engine_maintenance`. `total` is exactly their sum and excludes Device
wait. The nested `decode` object reports the request's decode-class Host exposure, Device wait, and
round count; `units` reports its prefill/control unit counts. In a compact batch every participating
request is delayed by the full round, so these values explain request latency but **must not be
summed across concurrent requests**.

The JSONL file contains no prompt, message, tool payload, generated response text, raw client
identifier, username, authentication token, or API-key value. Process argv and model/log paths are
also omitted. When supplied, only the validated client session/request SHA-256 digests appear under
`request.client_identity`; API-key configuration is represented only as a boolean. The existing
stderr summaries remain available for operators but are rounded and are not the aggregation source.
Console lines use local
`[YYYY-MM-DD HH:MM:SS.mmm] [level]` timestamps. OpenAI Responses, OpenAI Chat, and Anthropic
generation requests receive a request ID when they enter synchronous preparation. Successful
preparation produces `request_start`; a preparation failure produces `request_rejected` without a
matching start. Later generation failures produce `request_error`. Schema/model validation
rejections before preparation and token-count-only calls are not measurement requests and do not
receive request IDs.

By default the server also reports aggregate activity every five seconds. `prefill` counts prompt
suffix tokens actually computed during the interval, excluding prefix-cache hits; `decode` counts
tokens finally committed by decode rounds, excluding the first token produced by prefill. For MTP
and DFlash this is the accepted committed output, not draft or rejected tokens.
`avg_decode_batch` is decode row-rounds divided by decode rounds during the same interval. The
`running`, `prefilling`, `decode_ready`, `waiting`, `materializing`, `capture_pending`, and
`terminal_pending` fields are the Engine scheduler snapshot at the end of the interval. The JSONL
`context_cache` object reports selection, capture, transfer, COW, pressure spill, private/shared
owner degradation and eviction, checkpoint drop, pressure search, budget exhaustion, maximal fallback, and historical-fork
counters as interval deltas; `occupancy` and `last_selection` are end-of-interval gauges. Materialization predictions are
request-owned and appear only on the corresponding `request_done` event.
`pressure.searches` counts plans accepted into Program resource transactions, including a transaction that later ends in
request-local abort; committed victim counters likewise report the resulting stable cache changes.

The JSONL `throughput.host_work` object is the aggregation authority: the Engine worker counts each
wall-time segment once, independent of batch size. `elapsed_seconds` contains the same five
mutually exclusive Host phases and their `total`; `device_wait_seconds` is separate.
`work_class_seconds` splits Host and Device-wait time into decode, prefill, and control classes.
`detail_subset_seconds` and `detail_invocations` expose admission, context-transaction, replica, and
stats-publication slow paths; these detail values are already contained in a top-level Host phase
and must not be added to `total`. Per-round, per-row-round, and per-invocation normalized values are
`null` when their denominator is zero. The stderr interval line shows only total Host milliseconds,
decode Host/device-wait microseconds per round, boundary, and maintenance; use JSONL for analysis.
Intervals with context materialization or retention activity are retained even when they contain no
token execution; only fully idle intervals are omitted. Downstream measurement should prefer the
raw counters and seconds over rounded stderr rates.

## Execution behavior

The server owns one resident Engine with a startup-fixed capacity of `1..8` active generation
requests. At each decode boundary, every decode-ready request is compacted into one batch and
processed by one model traversal and, when graphs are enabled, one exact-batch CUDA Graph replay. A
request joins that batch only after its single-request prefill finishes; when it completes or is
cancelled, the next boundary rebuilds the batch without an empty row.

`--max-pending-requests` bounds the requests waiting behind the active set. The total generation
request lifetime capacity is `max_concurrency + max_pending_requests`, including requests still in
CPU/media preparation and completed model results whose response has not yet been released. A full
capacity returns HTTP 429 with code `server_overloaded`. The absolute
`--pending-timeout-ms` deadline starts before preparation, covers media acquisition and Engine FIFO
waiting, and returns HTTP 503 with code `request_queue_timeout` if admission does not occur in time.
There is no admission ETA or unbounded overflow queue.

Input memory is bounded by the outstanding-request count and the per-request
`--max-request-mib` limit. Media requests additionally share one preparation permit, so a waiting
media request retains the same cancellation and timeout deadline. Model output is bounded by the
same finite request count and each request's effective output-token limit; output callbacks and
network serialization run outside the GPU executor and do not delay formation of the next batch.

`--max-context` is each sequence's logical ceiling. `--kv-capacity` fixes the shared Main Text KV
pool used by active requests and retained prefixes. `auto` accounts for the complete enabled runtime
and leaves 1 GiB of sizing headroom; omitting the option makes it follow `--max-context`. Capacity
resolves once at startup.

Admission reserves the full prompt-plus-effective-output page entitlement through request
completion. A request remains queued until a legal resource plan can satisfy that entitlement.

Each reusable checkpoint contains KV and complete continuation state. At admission, capture, and
finish boundaries, resource pressure may keep it on Device, move its StateImage and/or KV replicas
to pinned Host memory, or evict it. The planner compares incoming-request work with the later
recovery cost imposed on retained checkpoints. Active requests retain their state and completion
reservations, and placement choices preserve model semantics. The full policy and invariants are
defined in [Resource scheduling and context cache](maintainer/resource-scheduling-and-context-cache.md).

Compatible prefixes are reused for both text and multimodal histories unless the server starts with
`--no-prefix-reuse`. A multimodal hit additionally requires matching token types, three-axis MRoPE
positions, encoded-media digest, grid, and consumer spans. Media wholly inside a matched prefix
skips Vision execution, while new suffix media is encoded normally. The completion log reports the
reused token count as `cache=`.

The completion log reports one of six reuse paths: `root`, `private_endpoint`,
`private_turn_closure`, `private_response_replay`, `private_long_anchor`, or
`shared_stable_prefix`. Reuse validation covers KV, recurrent state, hidden state, selected-backend
state, and the exact prompt frontier. With stable `preserve_thinking=true`, the auxiliary checkpoint
rolls to the message frontier immediately before the current response's deterministic generation
prologue. A normalized response, compact-summary instruction, or replacement user suffix therefore
replays the small generation prologue and only the changed suffix while retaining the complete
stable conversation prefix. Stable `false` places the turn-closure checkpoint before the first
assistant opener in the open turn, so closing that turn can recompute its opener and omit its
reasoning without discarding the preceding conversation.

`preserve_thinking` selects the capture frontier for newly created checkpoints. Existing exact
checkpoints remain reusable across a mode change. If the desired boundary is behind the selected
reuse frontier and has no snapshot, the Engine keeps the valid hit and defers the new checkpoint. A
later request that diverges before every retained checkpoint starts from root. The JSONL completion
record exposes the restored checkpoint as `prefix_reuse_path`. Reasoning-effort changes participate
in rendered-token identity and exact-prefix selection.

An appended mid-conversation system message is an ordinary prompt suffix, so an unchanged prior
history remains eligible for `private_endpoint`. If the client modifies, removes, or moves a
historical system message, the token prefix genuinely differs and a miss/reset is correct.

Speculative backends preserve protocol output shapes, stop behavior, and usage accounting. If a stop
truncates a multi-token MTP or DFlash round, the Engine commits the exact accepted target prefix so
a following compatible turn can reuse it. Output-limit and context-capacity finishes map to
`length`/ `max_tokens`; ordinary model or string stops map to `stop`/ `end_turn`.

Function tools are rendered into the model prompt and generated calls are parsed into protocol
responses. NInfer does not execute tools and does not validate tool arguments against the full
client JSON Schema through constrained decoding; that remains the client's responsibility. When
parsing a generated call, NInfer does consult the top-level parameter `"type"` declared in each
tool's schema to decide whether a parameter value that is valid JSON may be deserialized into the
corresponding JSON type (number, boolean, array, object, null): only parameters whose declared
type(s) are all valid non-string JSON Schema types are deserialized. Parameters typed as `"string"`
(or declared via a type array that includes `"string"`), parameters with an unknown or misspelled
`"type"`, and parameters absent from the schema preserve the model's raw text so the string
contract reaches the client intact. Full JSON Schema validation (constraints, required sets,
formats, nested keywords) is not performed server-side and remains the client's job.
Duplicate tool names within a single request are rejected with a 400 on all three
protocol surfaces (OpenAI Chat Completions, OpenAI Responses, Anthropic Messages),
keeping the per-tool parameter type map unambiguous.

Prompt-token usage includes chat-template and expanded media tokens. Generated-token usage comes
from accepted output token IDs, including a stop token whose decoded text may be withheld.
