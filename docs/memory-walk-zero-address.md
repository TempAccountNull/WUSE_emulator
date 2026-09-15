# Memory walk starting at zero

The actual game queried `NtQueryVirtualMemory(-1, 0, MemoryBasicInformation,
buffer, 48, ReturnLength)` twice in the worker-memory-classification replay.
Both calls returned `STATUS_INVALID_PARAMETER` to `destiny2.exe+0x282921`.
The failure occurred before any region could be returned.

The matched `ntoskrnl.exe` 10.0.19041.7417 accepts this query. Its upper-bound
check is at RVA `0x68871A`; there is no allocation-minimum check. The free-range
branch rounds the query down to a page and computes the distance to the next
VAD, or the user-address limit. It does not substitute a fixed 64-KiB size.

Sogen already calculates that range in `memory_manager::get_region_info`.
The syscall now allows low addresses for BasicInformation and
PrivilegedBasicInformation and reports free `AllocationProtect=0`.
The other free fields remain `AllocationBase=0`, `PartitionId=0`,
`State=MEM_FREE`, `Protect=PAGE_NOACCESS`, `Type=0`. Free-result padding bytes
at offsets 22-23 and 44-47 remain untouched, as in the matched kernel.

## Validation

All five new address-walk regressions failed before the fix and pass afterward.
Including the prior thread-information coverage, 32 Icicle tests passed in
4.379 seconds. Tests cover zero and unaligned low queries, reaching the next
allocated region, a known free hole, the upper address boundary, both basic
classes, exact fields, untouched padding and output/ReturnLength widths.
VS2022 RelWithDebInfo and the required tidy configuration were built.

Kernel SHA256:
`c3c0dc9599964633d8958558e79051e50278cbca998a4ce2b5b769134b762eda`.
IDA port 13379 matched that exact file. Wine and ReactOS were read for comparison;
their validation-order differences were not treated as current Windows proof.

Evidence under `D:/source/repos/test_research/PatchScanner/artifacts/source-crosschecks`:
`memory-walk-baseline-tests.json`, `memory-walk-fixed-tests.json`,
`nt-query-virtual-memory-basic-kernel-contract.md`, and its retained IDA/raw-byte
companions. Actual call arguments and returns are in capture
`destiny-sogen-1789514265155431200-worker-memory-classification`.

## Replay limit and remaining work

That capture ended after a debugger-step timeout. Sogen then reported
`bad allocation` while attempting to write a snapshot; no new snapshot exists.
This is not evidence of a guest exit or successful boot. The original parent
checkpoint and the complete focused, printed and suspicious journals survive.

The observer now retains the pending response across socket timeouts, including
partial payload/checksum reads. Four transport regressions pass. The
high-frequency NtClose breakpoint was removed from this diagnostic observer.

The actual-game replay `destiny-sogen-1789515449358765600-zero-memory-walk-fix`
returned STATUS_SUCCESS at the same caller, `destiny2.exe+0x282921`. It returned
48 bytes, BaseAddress=0, RegionSize=0x10000, MEM_FREE, AllocationProtect=0,
Protect=PAGE_NOACCESS and Type=0. The captured result bytes have SHA256
`5a3c7bdd033b4fed1520e75bd5ac05d04e03eff67ea2a17c79533d764a145995`.
The saved checkpoint before the caller consumes the successful result has SHA256
`5ec1965ebacd413bfb1a413124f68f300418bb9ad8784dec35de557f38336112`
(324,038,739 bytes). The run ended by the requested debugger disconnect after
saving, not by a guest exit. The formerly failing query is verified fixed in the
actual game; its relationship to WARP worker selection remains unproven. Broader query-contract gaps (probe ordering, short-buffer
status, full protection metadata and process-handle access) remain explicitly
documented in the kernel-contract audit; this change is an address-walk fix,
not a claim that all NtQueryVirtualMemory classes are complete.
