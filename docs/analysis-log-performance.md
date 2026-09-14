# Analysis log writes

Per-event JSON flushes and separate console color resets stall the emulation
thread on file I/O. A sampled application replay spent 856 of 2,597 samples
(32.96%) inside NtWriteFile.

Write JSON on a background thread in 64 KiB batches. Two 1 MiB buffers bound
storage; a full producer buffer applies backpressure. Sparse output flushes
every 250 ms. Explicit flush waits for queued writes and propagates I/O errors;
destruction drains the remaining buffer. Forced termination can interrupt
pending output. Event formatting, order and selected categories are unchanged.

On Windows with FORCE_COLOR, combine the ANSI prefix, message and reset into
one write and immediate flush. Embedded color spans remain intact. Configure
FORCE_COLOR before starting the process.

Two application snapshot replays per build reached the same traced-call
checkpoint in 28.653/29.046 seconds before and 21.304/21.218 seconds after.
Mean elapsed time fell 26.30%; throughput increased 35.69%. Both builds used
JIT, verbose logging and instruction precision. These timings exclude snapshot
restore/save and measure one startup segment, not full application boot.

Each stream contains 1,916,910 non-progress events with matching type order and
counts. Before/after register packets and sampled memory match. Instruction
counts and five thread-switch locations differ; one repeated comparison also
differs in one module-list member access. Preserve those differences in the
runtime evidence rather than claiming identical execution traces.

Validation: 33 focused tests pass in interpreter and JIT modes, including
ordered records across buffer boundaries, oversized/binary records, destruction
drain, sparse flush and invalid paths. Release and tidy builds pass. The CLI
smoke still reaches the pre-existing C000041D Window Geometry failure.
