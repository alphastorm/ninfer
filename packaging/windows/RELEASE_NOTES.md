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
pool is its largest charge. A pin charges its size and then a page-lock remainder of 11-20 MiB;
when free commit covered the size but not the remainder, the pin raced the pagefile extension, and
a start that lost was refused with 10 MiB of commit free. Before every pinned host allocation the
server now commits and releases its size plus 1/64 through an ordinary allocation, which waits for
the pagefile to extend, so the pin itself never waits on an extension. A refusal that still occurs
reports the commit limit, available commit and available memory.

Decode rounds are shorter. The MTP verify pass's small-extent Q4 and Q5 input projections share
each activation load across weight rows, and the Q4 MLP gate/up kernel pads its staged weight rows
so its shared-memory reads no longer conflict. Each row keeps its arithmetic order. On the RTX 5090,
where the routes were tuned, outputs are bit-identical to the previous kernels and MTP3 decode is
about 10% faster from 1K to 31K tokens of context. The native lanes build these routes for their
architecture, except that the MLP down and mixer output projections keep one row per warp: sharing
loads across two rows made them 2-22% slower on the RTX 4090.

New sessions after idle start at back-to-back speed on the RTX 4090. After its last work the card
steps P2 -> P3 -> P5 -> P8 (210 MHz SM, 405 MHz memory) within about 6 s, and a new session's
prefill after 12-58 s idle took 0.29-0.47 s against 0.146 s back to back. The RTX 4090 lane sets
`engine.gpu_keep_warm_ms` to 60000: for 60 s after the engine goes idle, a single-warp kernel on
its own stream spins 50 ms of every 100 ms, which holds P2 at about 72 W above idle, until a
request is pending. The RTX 5090's pattern of 3.5 ms every 10 ms does not hold this card. With it,
sessions after 12-58 s idle prefilled in 0.146 s and every output was unchanged (omp-ninfer EXP-064,
EXP-066). The spin reads and writes no memory. The controller passes `--gpu-keep-warm-ms` only when
a release's own configuration declares a positive value, so a rollback to an earlier release never
receives it. The RTX 3090 lane declares 0.

Long sessions keep their cache. OMP's compaction handoff resends the whole session with
`tool_choice: none`, and the runtime rendered that turn without the declared tools, which Qwen
places before the leading instruction: every handoff re-prefilled the session from root (63.8-66.3 s
to first token for 97.4-97.5K tokens on the RTX 4090). The tools now render for `none` as for
`auto`, and the template's tool-call opening token ends the turn instead. Handoffs of 78.4-80.4K
tokens reused 78.0-79.9K cached tokens and started in 0.58-0.70 s, and their summaries ran
3,235-5,241 bytes.

Near capacity the materialization planner's 5 ms search can stop before it reaches a target that
keeps the session's own continuation. It then fell back to evicting that continuation and
prefilling the whole turn again from root (about 102,600 tokens and 68 s on the RTX 4090,
omp-ninfer EXP-072). The fallback now starts from the cheapest feasible target that keeps it: in
three such stops during a long session, each turn reused 79.2-101.6K cached tokens.

A crash after a compaction restored the pre-compaction checkpoint. The compacted session's turns
stayed under the 32,768-token automatic-save minimum, and an unstored handoff under the session's
key could move its indexed lineage. A session that has a checkpoint on disk now keeps it current at
any size, and an unstored Responses turn leaves the lineage alone. After a hard kill, the next turn
reused 32,167 cached tokens in 2.9 s; the previous runtime prefilled 32,173 tokens from root
(29.2 s).

Long sessions keep their checkpoints when short sessions fill the store. Quota reclamation took the
oldest checkpoint first, so a run of short sessions could delete every long session's checkpoint:
about 26 short OMP sessions of about 930 MiB each filled the RTX 5090's 24 GiB store. Checkpoints
below `--session-checkpoint-min-tokens` now go first. On the RTX 4090, a 60,026-token session
(2.30 GiB checkpoint), then 11.9K-token sessions (702-704 MiB each) past the 24 GiB quota, then a
crash: the long session's next turn reused 60,057 cached tokens in 3.1 s. The previous runtime had
reclaimed its checkpoint first, and the same turn failed with `previous_response_not_found`.

A controller action no longer fails when the server is writing a checkpoint. Every action walks
the lane's state tree, and the walk could reach a checkpoint staging directory the server had just
renamed into its generation (`Cannot find path ...\.staging-...`). An entry that disappears during
the walk is now skipped; any other failure still stops the action.
