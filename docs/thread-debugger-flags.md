# Thread debugger flags

`NtSetInformationThread(ThreadHideFromDebugger)` and its query now use the
requested thread. Previously both resolved the requested handle, then used the
calling thread's flag. The setter also incorrectly accepted a BOOLEAN input
that could clear the flag; the query discarded output-write failures.

## Matched contract

Authority: guest `ntoskrnl.exe` 10.0.19041.7417, confirmed in IDA at port 13379.
SHA256: `c3c0dc9599964633d8958558e79051e50278cbca998a4ce2b5b769134b762eda`.
Image base: `0x140000000`; all addresses below are RVAs.

| Path | Kernel evidence | Implemented behavior |
| --- | --- | --- |
| Query entry `0x6EB0A0` | Probe path `0x6EB16D` through `0x6EB1B2` | Range/alignment validation, optional ReturnLength read/writeback, then class length and handle lookup |
| Query class 17 | Dispatch target `0x84531C` | Exactly one BOOLEAN; selected ETHREAD flag at `+0x510`, bit 2; output byte before ReturnLength=1 |
| Set entry `0x714260` | Alignment selector `0x715284`, branch `0x714331` | Zero-length pointer ignored; nonzero input checked for four-byte alignment and user range before length rejection |
| Set class 17 | Dispatch target `0x84C568` | Zero length only; atomically sets selected ETHREAD flag; no BOOLEAN payload or clear operation |

The one-byte query accepts byte alignment. ReturnLength points to a four-byte
ULONG and has no explicit alignment requirement. Its start is checked against
the user limit, then read access is evaluated before write access. This order
matters for an unaligned ULONG spanning read-only and guarded pages. A guard
fault clears only the accessed page's guard and returns its exception status.

ReactOS, Wine and XP source were read as references. ReactOS's stricter query
alignment and failed-query ReturnLength epilogue differ from this matched kernel;
XP lacks this query class. Those differences were not copied into Sogen.

## Validation

- Existing production code failed all 18 initial new regression cases.
- Final Icicle run passed 22 debugger-flag cases plus five existing ideal-processor
  tests: **27/27**. Elapsed test time: 3.774 seconds.
- Cases cover independent caller/target flags, ignored zero-length pointers,
  rejected BOOLEAN setters, exact output widths, unaligned and overlapping
  outputs, error ordering, read-only/unmapped/guarded pages and snapshot restore.
- Native handlers were tested with buffers below and above 4 GiB. This does not
  establish execution of the WoW64 thunk or full x86 process support.
- VS2022 RelWithDebInfo build and required tidy configuration completed successfully.
  Changed C++ files were clang-formatted.

Evidence under `D:/source/repos/test_research/PatchScanner/artifacts/source-crosschecks`:
`thread-hide-kernel-identity.json`, `thread-hide-kernel-disassembly.json`,
`thread-hide-ida.json`, `thread-hide-baseline-tests.json`,
`thread-hide-fixed-tests.json`, and the ReactOS/Wine source-contract audits.

## Remaining scope

The matched kernel requires query access `0x40` and set access `0x20`.
Sogen's public thread handles still share the internal object store and do not
retain granted access per handle. This change corrects target selection and
class-17 buffer semantics; it does not claim complete NT handle/security behavior.
The required ownership/access work is described in `thread-handle-contract.md`.

The observed game worker-selection path did not query class 17. These passing
tests do not prove that the WARP worker termination or white screen is fixed.
