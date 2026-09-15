# WARP worker termination: captured arguments

The guest, not the debugger, terminated worker `0x0C`. The selecting condition
is still under investigation. No termination bypass or handle-table fix was
applied in these replays. This is diagnostic evidence, not game boot acceptance.

## Before and after the syscall

The analyzer and Icicle backend are the validated address-size/AF build recorded
in `icicle-address-size-replay.json`:

- Analyzer SHA256: `1200297d51218614c9112c0e9a73dc0f9e2310c7a18e19ee7cb1244c2c128f19`.
- Backend SHA256: `0cb26a6a7debd69f0dcb638f7d4d0dde0cc67a1373482d31a680923bd7603903`.
- Guest execution: `--backend icicle -v -e D:\Sunrise\D2\sogen\root`, with the
  authorized existing `d:\sunrise\d2` mapping.

Capture `destiny-sogen-1789509290399387200-warp-worker-termination-capture`
stopped at the requested breakpoint **before** `syscall`. Its authoritative
`held-address.json` records `T05thread:1c;`, state `00020`. The breakpoint fired
before the later checkpoint request was processed.

| Field | Captured value |
|---|---|
| Executed instruction count | 201,818,790,213 |
| Calling thread | `0x1C` |
| RIP | `0x12B3F2D0B2` |
| RAX | `0x53` (`NtTerminateThread` in the matched ntdll) |
| RCX / R10 | `0x4800002`, identifying live worker `0x0C` |
| RDX | `0`, requested exit status |
| RSP | `0x1378A2E518` |
| Return address at RSP | `0x140293793` = `destiny2.exe+0x293793` |
| Caller instruction | `destiny2.exe+0x293790`: `call r8` |

The gate starts at `0x12B3F2D0A0` in an observed private allocation:
`0x12B3E90000 + 0x9D0A0`, allocation size 1,155,072 bytes. Its 24 bytes match
the guest ntdll export at RVA `0x9E0A0`. The relocation bias used to compare
copied code is not an image base: the allocation begins 0x1000 bytes later and
does not establish a mapped ntdll PE image.

```asm
4c 8b d1                 mov r10, rcx
b8 53 00 00 00           mov eax, 53h
f6 04 25 08 03 fe 7f 01  test byte ptr [7ffe0308h], 1
75 03                    jnz +3
0f 05                    syscall
c3                       ret
cd 2e                    int 2eh
c3                       ret
```

Checkpoint SHA256:
`5eadcea32bc999bd9aafd182a7587928ac180b16f4fe1131ef1d01f484376c64`
(335,914,025 bytes).

Capture `destiny-sogen-1789510887590979400-presentation-boundaries` resumed that
checkpoint unchanged. Its first syscall event at instruction 201,818,790,214
is the captured `NtTerminateThread`; the next event identifies terminated TID
12, caller TID 28, exit 0. This connects the saved arguments to the actual
termination. The replay ran 792.998 seconds and saved checkpoint SHA256
`e9416e846da272197415f4e136d3b5b26c7c6c7baf035f6c852784de64584e2c`.
Analyzer exit 1 followed the debugger checkpoint disconnect; it is not evidence
of a guest exit or successful boot.

## Handle selection remains separate evidence

The pre-call stack contains `0xFFFFB000B8000010`, Sogen's synthetic identity for
process handle `0xB800001`. Candidate pointers `0x148D750000` and `0x148D91DEE0`
are outside the snapshot's mapped regions. A streaming read of known stack and
gate bytes agreed with the debugger's saved bytes; the absent candidate buffer
cannot be reconstructed from this checkpoint.

Replay `destiny-sogen-1789512192899357600-worker-handle-selection` starts from
the earlier checkpoint SHA256
`43cd45693d4f89783a6d2632accf06f8440a0c02d45bbb3eb3cc80114465307a`.
The observer verifies the guest ntdll CodeView identity and all 24 bytes of each
selected original/copied syscall gate before setting breakpoints. It records
x64 arguments, return sites, statuses and query buffers for thread `0x1C`.

Its first captured successful `SystemExtendedHandleInformation` query returned
1,575 entries (63,016 bytes), to `destiny2.exe+0x27F25B`. The table contains
process handle `0xB800001` and its synthetic identity, but **does not contain
worker handle `0x4800002`**. This table therefore does not establish the route by
which the game acquired the worker handle. The query buffer SHA256 is
`c04accff6e4b6b82f5a017d43a912172add0470751809d26593e575fe2b8445b`.
The replay completed at the pre-termination checkpoint described below.

Independent source review found that internal thread ownership, public handles,
access rights, duplication and close semantics share one object store. The
required model and tests are in `thread-handle-contract.md`. That source defect
does not by itself prove this guest's selecting condition. Old snapshots lack
historical access and alias metadata; do not fabricate those values during a
purportedly faithful replay.

Matched IDA/guest ntdll SHA256:
`e1ff52ee55a24dc03923f37c325d161e43626f5d43a58d5e16488e998202ed91`;
CodeView GUID bytes `ea8d6dde4bf185c3e14d71f138cf8ad7`, age 1.
The worker's start address, ntdll+`0x4D110`, is `TppWorkerThread` in that database.

## Presentation observations and preservation

The bounded presentation observer captured 27 `NtUserGetMessage` entries,
26 returns, and one `NtUserMessageCall` entry/return. Returned messages included
`WM_ACTIVATE` and `WM_MOUSEMOVE`. No selected Present or pixel-transfer boundary
was hit. The complete 4,071,369,405-byte event stream contained the one worker
termination and no selected Present/Blt/CreateDCFromMemory/EndPaint syscall.
These observations do not prove that every rendering path was observed.

The guest DXGI Present entry has a JMP patch; its 32-byte guest/disk comparison
is retained in `presentation-boundaries.jsonl`. Watching that entry alone cannot
exclude a hook calling the original implementation through a separate trampoline.

One suspicious-event collector encountered a host `MemoryError` during the run.
After the run ended, it resumed from its persisted offset and reached the exact
end of the source stream: 4,071,369,405 bytes, zero pending bytes. Both suspicious
and printed journals were complete before bulk cleanup. The original collector
error remains recorded; recovery is not reported as an uninterrupted collection.

The two completed captures' cleanup manifests retain checkpoint hashes,
first/last log samples, selected lifecycle/query observations and both journals.
Their removed bulk logs total 29,843,027,325 bytes. Active replay logs were kept.


## Worker selection captured

The completed worker-handle-selection replay saved checkpoint SHA256
`e2a410b81692b669f9e8729ef3d93f666e394b009cf2abe5c1b9038bc111580c`
(335,618,974 bytes). The debugger stopped at `0x12B3F2D0B2`, before the copied
NtTerminateThread syscall, on thread `0x1C`.

The focused argument/return journal establishes this sequence for worker TID 12:

| Operation | Game return RVA | Observed input/result |
| --- | --- | --- |
| NtOpenThread | `0x26DFCB` | CLIENT_ID process 4/thread 12, access `0xB`; Sogen returned handle `0x4800002` |
| NtSuspendThread | `0x290194` | Success, previous suspend count 0 |
| NtGetContextThread | `0x267514` | ContextFlags `0x100011` (CONTROL and DEBUGREGS) |
| NtResumeThread | `0x289554` | Success |
| NtQueryInformationThread | `0x2784D4` | Class 9; success, start address `ntdll+0x4D110` (TppWorkerThread) |
| NtTerminateThread | `0x293793` | Handle `0x4800002`, exit status 0; saved before execution |

The first two captured worker contexts had RIP `0x1800A10E4`, in ntdll's wait
path. The third had RIP `0x148CF65784` and RSP `0x12B33CF0C0`, in generated WARP
code outside loaded modules. Termination followed that third sample. This is
correlation; the guest's intervening predicate has not yet been established.
The handle comes from NtOpenThread, not the earlier observed handle inventory.

The class-9 query succeeds in Sogen although access `0xB` lacks query right
`0x40`. The matched 19041.7417 kernel's query branch requests `0x40` from
ObReferenceObjectByHandleWithTag. This is a concrete permissions mismatch;
whether correcting it changes the termination decision remains unproven.

No class-17 query was observed in this selection path. Independent class-17
target and buffer fixes, tests and remaining handle-model scope are documented
in `thread-debugger-flags.md`.

The full focused journal and buffers, worker-selection summary, checkpoint,
printed and suspicious journals remain in the capture directory. Both collectors
reached source offset 12,242,496,809 with zero pending bytes. After preserving
SHA256 hashes and first/last samples, completed raw console/event logs totaling
18,059,480,914 bytes were removed. No active-run output was removed.
