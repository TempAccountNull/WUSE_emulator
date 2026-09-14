# Analysis log writes

Per-event JSON flushes and separate console color resets stall the emulation
thread on file I/O. A sampled application replay spent 856 of 2,597 samples
(32.96%) inside NtWriteFile.

Write JSON on a background thread in 64 KiB batches. Two 1 MiB buffers bound
storage; a full producer buffer applies backpressure. Sparse output flushes
every 250 ms. Explicit flush waits for queued writes and propagates I/O errors;
destruction drains the remaining buffer. Forced termination can interrupt
pending output. Event formatting, order and selected categories are unchanged.

On Windows with FORCE_COLOR redirected to a regular file, batch the ANSI
prefix, message and reset on a second writer thread using the same buffer limits.
The worker borrows stdout and never closes it. Forced messages wait for a flush;
destruction drains pending output. Native consoles and pipes remain synchronous.
Embedded color spans remain intact. Configure FORCE_COLOR before construction.
The two writers together reserve at most 4 MiB of queued buffer capacity.

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

Validation: 37 focused tests pass in interpreter and JIT modes, including
ordered records across buffer boundaries, oversized/binary records, destruction
drain, sparse flush, invalid paths/streams, borrowed-stream lifetime, every
console color, embedded highlights and silent logger sink delivery. Release and tidy builds pass. The CLI
smoke still reaches the pre-existing C000041D Window Geometry failure.

After the JSON change, another profile attributed 556 of 2,608 emulation-thread
samples (21.32%) to NtWriteFile. Two unprofiled application replays per build
measured console batching: 20.722/21.013 seconds before, 16.991/17.288 after.
Mean elapsed time fell 17.86%; throughput increased 21.75%.
The same snapshot, call checkpoint and tracing settings were used. Each stream
again contains 1,916,910 non-progress events. Types and order match; differences
are limited to instruction counts and five thread-switch locations. Captured
register packets and sampled memory match. One fewer wall-clock progress line
accounts for the differing console line and ANSI-span counts.

Continuing from the new checkpoint reproduces the earlier saved exception
record and context exactly. Application boot remains unconfirmed.
