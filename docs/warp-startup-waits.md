# WARP startup waits

The user confirmed a white window after the address-size and AF fixes. Actual
render-thread observations identify a lock dependency; they do not establish a
display-backend failure.

Capture: `destiny-sogen-1789507727869529400-address-size-and-af-fix`.
Analyzer SHA256: `1200297d51218614c9112c0e9a73dc0f9e2310c7a18e19ee7cb1244c2c128f19`.
Saved checkpoint SHA256: `741b6c3800a4e893cb89d5da7b48f984648199391e02b69239672579d1b0791e`.

The exact guest WARP DLL and IDA database agree on SHA256
`ac35b849b483fc9196f24138a731c0739fc2f30b04792931de72cc5a7ce3053f`.
Guest image base is `0x91B1A0000`. Its CodeView GUID bytes are
`1718da83ea57a0d265039e810546b3aa`, age 1.

| Observation | Recorded evidence |
|---|---|
| Thread `0x5C` first waits for messages | Saved states 00002-00004: 344,525,895 instructions, `wait_message=1`, queue empty. |
| It later blocks entering a critical section | State 00005 onward: 344,540,296 instructions, `wait_alert=1`, no pending alert or timeout. The last function call is `RtlEnterCriticalSection`, from WARP+`0x6AB53`. |
| The lock is WARP's `gDDICriticalSection` | Matched PDB name at WARP+`0x6B4C90`. Saved memory at `0x91B854C90`: LockCount=-6, RecursionCount=1, OwningThread=`0x48`, LockSemaphore=`0xFFFFFFFFFFFFFFFF`, SpinCount=`0x20005CF`. |
| The caller is creating a render-target view | WARP+`0x6AB20`: `UMDevice::CreateRenderTargetViewInternal`; its first critical-section acquisition is the recorded call. |
| Owner `0x48` waits on an event | Saved registers: `R10=0x1800056`, `R8=0`, RIP=`ntdll+0x9D6D4`, inside `NtWaitForSingleObject`. The saved return-address slots include WARP+`0x34D294`, following the wait call in `ThreadPool::WaitWhileBusy`. Raw stack candidates are retained; this was not a complete Windows stack unwind. |
| WARP worker `0x0C` was terminated earlier | At instruction count 186,761,799,748, thread `0x1C` issued inline syscall 0x53 (`NtTerminateThread`) at `0x12B3F2D0B2`. The following thread-termination event names target `0x0C`, exit 0. Its last recorded execution was in WARP+`0x306AE7`. |

The terminating thread was different from the worker. The initial termination
event did not contain its handle argument, return address or selecting caller;
those require a replay stopped before the call. Do not infer why it selected
that worker, bypass the call, or fabricate completion of its unfinished work.

The active thread-query implementation does not switch guest threads. A separate
paused snapshot inspector was used for `Hg` selection and saved register reads,
because selecting a thread can run scheduler bookkeeping. That inspector never
continued or stepped the guest; its process was closed after the reads. The live
replay resumed the untouched checkpoint using the same analyzer and explicit
`-e D:\Sunrise\D2\sogen\root -v` options. Its viewer retained latest-tail mode,
ANSI colors and saved geometry (-5,1050,1126,341).

Full initial event-stream inspection found 146 `NtWaitForAlertByThreadId`
function entries and 145 `NtAlertThreadByThreadId` entries. These aggregate
counts are not a proof of a lost wake. Sticky alerts and wait entry are protected
by the same emulator kernel lock, and their state is saved in snapshots. The
captured wait's actual lock owner must be followed instead.

Named `D3DKMTCreateDCFromMemory` strings in that capture were GetProcAddress
arguments from DXGI+`0xC750`, not transfers of pixel data. Matched DXGI
SHA256 `d33765793e32eff65f9103efe2419184ab0b6289e69e69ce85aa7568f678c3b5`
and CodeView GUID bytes `fd38804e071a44a0c0137e92aee4e84e`, age 1, identify
`CDXGIBaseAdapter::LoadThunks` at RVA `0xC4B0`. The earlier working note
used an incorrect RVA `0xCC750`; subtracting the guest base `0x2DB11C0000`
from VA `0x2DB11CC750` gives `0xC750`. No selected BitBlt,
StretchBlt, CreateDCFromMemory, DdDDIPresent or EndPaint execution event was found
by that full-stream query. WARP rasterizer samples prove guest execution, not a
visible frame.

Evidence under PatchScanner:
- `benchmarks/<capture>/render-thread-final-events.json`
- `benchmarks/<capture>/render-wait-events.json`
- `benchmarks/<capture>/warp-worker-12-final-events.json`
- `benchmarks/<capture>/thread-lifecycle-events.jsonl`
- `benchmarks/sogen-snapshot-threads-1789508872096090700/saved-thread-5c.json`
- `benchmarks/sogen-snapshot-threads-1789508953954823500/saved-thread-48.json`
- `benchmarks/sogen-snapshot-threads-1789508953954823500/memory-91b854c90.json`
- `artifacts/source-crosschecks/warp-critical-section-functions.json`

Completed bulk console/event files were removed after checkpoint verification.
Cleanup manifests retain hashes and first/last samples. The journals, selected
thread evidence and checkpoint ancestry remain. Boot acceptance is not achieved.
