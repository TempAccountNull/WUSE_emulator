# Bounded checkpoint saving

The actual Destiny run could reach a debugger checkpoint but failed to save it
with `std::bad_alloc`. The old writer retained the serialized guest state, a
region-sized temporary, the compression output allocation, and a second copy
of the compressed output.

File snapshots now count the serialized size, read guest memory in 64 KiB
chunks, and stream one Zstd frame to a temporary file. The existing SNAP v1
header and field markers remain unchanged. Publication replaces the requested
file only after compression, flush, and close succeed. Failure removes the
owned temporary file and preserves an existing checkpoint.

A deliberate debugger capture is reported as `Guest paused ... checkpoint
saved`, with a `state: paused` footer and no invented guest exit status. A host
snapshot-save failure has a separate `snapshot_save` phase. Backend faults,
unhandled execution exceptions, and other failure stops are not successful
checkpoint pauses.

## Actual-game validation

All runs used Icicle JIT, `-v`, an explicit guest root, and the existing game
directory mapping. The comparison started from the same 333,789,086-byte
checkpoint, SHA-256
`7d7454550a613d5fb1671b1b631ad02765d98f105b599476209c4e342f408bd8`.

| Observation | Previous writer | Streaming writer |
| --- | ---: | ---: |
| Peak process private memory | 14.886 GiB | 10.609 GiB |
| Peak working set | 8.941 GiB | 9.480 GiB |
| Snapshot outcome | Allocation failure; no file | 334,777,377-byte file saved |
| Whole bounded run | 194.122 seconds | 209.998 seconds |

Private memory fell 28.73%. Working set increased. These runs are not a
speed comparison: the old run failed before publication, and equal observation
durations do not imply equal instruction states.

The saved frame declares 3,370,120,343 uncompressed bytes. SHA-256:
`27849dc8aaa6f2a6dd7eb1e1c74e67a2ef544ed53cdacc4f7114eb4e6356f5c0`.
On restore, the complete debugger register packet, parsed integer/floating
registers, and sampled code, stack, frame, and pointer memory matched the
capture before any guest execution. Execution then continued for 30 seconds
and saved another checkpoint with host exit 0 and a paused footer. Its SHA-256:
`2559159b380add8e24c2b6842ebc87d1c1f890d3204dba7314bfe75da820440b`.

This proves file creation, compatibility, sampled restored-state equivalence,
and continuation. It does not prove that every restored byte was compared or
that the game booted. The user last reported a white window.

## Tests and remaining limits

The focused snapshot, observation, and memory-walk and debugger-stop suite passed 29 tests.
Coverage includes legacy field markers, a 5 GiB counting pass without guest
reads, integer overflow, empty and chunk-boundary payloads, known-size single
Zstd frames, failed writes, failed publication, and pause/error reporting.

Four broader `SerializationTest` tests failed because the sample guest exited
with status 1. Two fail before their first serialization call. Their failure
cause has not been established; these results are not a passing full suite.

The in-memory snapshot API remains buffered. Restore still holds the compressed
input and full decompressed state. Live host Vulkan objects cannot currently
be restored; GPU bridge serialization does not preserve their lifetime. A
checkpoint with live GPU objects is not proof of a valid DXVK continuation.

Local evidence is retained in PatchScanner's
`artifacts/source-crosschecks/snapshot-actual-game-validation.json`,
`snapshot-final-guard-tests.json`, and `snapshot-stream-tests.json`.

The RelWithDebInfo build and required clang-tidy build passed.
