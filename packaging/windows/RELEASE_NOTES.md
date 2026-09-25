# NInfer native Windows lanes

One runtime for the RTX 4090 (`sm_89`) and RTX 3090 (`sm_86`) lanes, built from mainline
`port/native-lanes-on-mainline` rather than the two divergent lane branches. The package carries
the same context cache, warm arrival across a restart, and streamed verified-or-refused restore
that ship on the RTX 5090 container lane, with Windows platform support, the DirectStorage read
queue, and the MSVC build of the host tree.

Each lane installs into its own isolated state root through `Install-Release.ps1`, runs one
authenticated request at a time on a loopback or Tailscale address under a scheduled task owned by
SYSTEM, and keeps durable session checkpoints under its per-release cache. The lane's exact
engine configuration is bound by hash into the build identity; the lane specification names the
GPU, architecture, power policy, and task identity the installer enforces.

The lane configuration sizes the pinned host pools (`context_cache.host_state_slots`,
`context_cache.host_kv_mib`) for the host that runs the lane. The RTX 4090 keeps the qualified
11,264 MiB Host KV pool, 24 host state slots, and 32,768 MiB runtime-host floor. A 4 GiB pool
could serve a ceiling-sized session but could not restore its checkpoint; startup now discloses
the restorable token bound and export refuses a session larger than that bound. The RTX 3090
profile is unchanged. No smaller-host deployment profile is qualified by this change.

When shared capacity blocks restore, the engine may reclaim another inactive resident session
whose checkpoint is current. The verified generation stays pinned against quota eviction for
the entire restore, including saves of later victims. A resident session ahead of its checkpoint
is saved and pinned first; a refused save leaves that resident intact. Every retry reads from a
fresh checkpoint reader. The retry bound allows two progress steps per catalog slot plus the final
import, so new arrivals cannot extend one restore indefinitely. State image slots, KV capacity,
and continuation slots all participate in guarded reclaim. Normal disk
quota retention still applies after the restore finishes.

Live sessions survive a graceful stop. An automatic checkpoint refused at a transient gate - the
newest turn not yet catalogued, or another request's transaction in progress - retries up to six
times per completed turn once the engine quiesces. Before admission evicts a session whose newest
turn is not on disk, the engine saves it, waiting while that turn's reply is still being stored;
the admitting request waits, bounded by its own queue deadline. Re-saving a session under the
disk quota no longer deletes other sessions' only checkpoints, including when a cleanup fails. A
graceful stop counts a session whose newest response is already on disk as nothing to save.
Automatic checkpointing remains best effort under live traffic; explicit checkpoint requests and
graceful managed shutdown provide the observable save outcome.

Unmodified OpenAI clients get durable sessions. A Responses or chat request's `prompt_cache_key`
becomes the session identity when API authentication is configured: the key is hashed under its
own domain and never stored raw, and a request that also sends `ninfer_session` or
`X-NInfer-Session` is refused on both endpoints. Without authentication the key names nothing and
the request stays in the anonymous pool; a null key is the same as no key. A client that resumes
after both it and the server restarted, and so replays its transcript without
`previous_response_id`, has its session's checkpoint restored on that session's first request
since the start; the engine's exact prefix match decides how much it reuses. This applies to every
session name, so a `ninfer_session` or `X-NInfer-Session` client that opens a new conversation
under a saved session name also imports that checkpoint first. Requests without a session identity
behave exactly as before. The endpoint still rejects reasoning summaries, encrypted reasoning
requests, and the other cache options it does not implement.

A tool result may be content parts instead of a string: a Responses `function_call_output` or
`custom_tool_call_output` whose `output` is an array of `input_text` and `input_image` parts - what
stock OMP sends after its read tool opens an image file - becomes one tool turn with its image.
This lane is text-only, so it refuses that image as `vision_disabled` rather than as a malformed
request; `input_file` parts and empty arrays are refused.

The shared-prefix catalog defaults to one owner per active request or per cache marker a request
may place, whichever is larger (four at one active request), so each agent type's system and tool
prefix keeps its own owner instead of evicting the previous type's.

A start could fail when the driver refused to pin the 11 GiB host-KV pool
(alphastorm/omp-ninfer#48): the lane's task exited before the release was ready. On a 32 GiB host
the running server commits about 39 GiB, so a start extends the system-managed pagefile, and the
pool is its largest charge. The server now commits the pool's size through an ordinary allocation,
which waits for the pagefile to extend, immediately before pinning it; a refusal that still occurs
reports the commit limit, available commit and available memory. The refusal was not reproduced
on demand, so #48 stays open until a start either pins or reports those numbers.
