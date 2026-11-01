# Restore timing-only derivative (never merge)

This branch is an experiment artifact for issue #59, not a runtime fix or release qualification.
The original source revision remains the branch parent. Apply the same instrumentation patch
to both preregistered source revisions; do not rebase either onto a newer runtime.

`restore_trace.h` emits fixed 256-byte records: nine little-endian 64-bit integers
(monotonic ns, wall ns, Linux TID, six event-specific values), a 64-byte event name,
and 120-byte text. Each recorder reserves 8 MiB and drains outside producer threads.
No event producer flushes to disk. `clock.start`, `clock.end` and `trace.dropped`
are mandatory. A nonzero drop count, truncated field, incomplete record, missing end,
collector error or unavailable required capture invalidates the relevant observation.
The first monotonic value is CLOCK_MONOTONIC-equivalent steady-clock nanoseconds on Linux;
CUPTI uses the registered same-clock timestamp callback. Do not mix with another host's clock.

Set `RESTORE_TRACE_FILE` for server timeline output. Load `restore-collector.so` before
CUDA initialization with `LD_PRELOAD` and set `RESTORE_CUDA_FILE` to a different file.
The collector records all runtime/driver API and resource callbacks without preloading
modules or inspecting module bytes; activity records cover copies, kernels, functions,
CUDA events and synchronization. Function names are losslessly segmented into `cuda.symbol`
records. `cuda.loading.effective` is queried after engine initialization and before warmup.
CUPTI fixed callback buffers use 8 MiB; its device pool is limited to four 1-MiB buffers.
The server recorder and collector recorder each use 8 MiB, below the 64-MiB logging cap.
Driver-internal allocations are not claimed as observer-owned buffers.

Join `server.instance`, `http.request`/`http.response` on the same HTTP thread,
`request.start.http`/`request.start.response` on request ID, and `request.engine`
to the engine ID. Request numbers restart with each process; never join them alone.
`durable.session`/`durable.generation` belong to the restore span on that thread.
Worker activity without an explicit request ID is thread/interval context, not proof
of target-request causation. Preserve original JSONL request logs and all raw trace records.
Nested spans are not additive. Observer clock/ID arithmetic lives in `contract.py`.

The private window controller must enforce the frozen store/log/time caps and stop
rather than downgrade tracing. `contract.py` is a transport component, not a host operator.
The GPU-free workflow checks recorder event order and ID correlation, transport deadlines,
L/F polling arithmetic, and compiles the CUPTI collector without running it.
No hardware/profiler capability or causal conclusion follows from that CI proof.
