# Debug print capture

The analyzer records arguments at `OutputDebugStringA/W`, `DbgPrint`, `DbgPrintEx`,
`DbgPrintReturnControlC`, `vDbgPrintEx`, and `vDbgPrintExWithPrefix` entry. x86 uses
four-byte stack slots, including four-byte pointers for `%I64n`; x64 uses RCX,
RDX, R8, R9 and eight-byte stack/va_list slots. Format strings and variadic values
are observations. The analyzer never evaluates the format or writes through `%n`.

Output records capture the guest-formatted payload presented through DBWIN,
`RtlRaiseException`, INT 2D service 1, and the matched x86
`NtWow64DebuggerCall(1, buffer, length, component, level)` path. The Unicode debug
exception carries UTF-16 and an ANSI fallback in one event. A transport observation
does not claim that a debugger consumed it. Filtering may produce a call without
an emitted payload; the analyzer does not invent one.

`debug_print_call.call_id` links to `debug_string.origin_calls` while the observed
call remains on that thread's stack. A resumed snapshot cannot reconstruct calls
that began before its recording; new calls acquire new capture-local IDs. Source
capture and byte offsets disambiguate IDs inherited across snapshots.

Raw bytes are retained as hex, including embedded NULs and UTF-16 code units.
ANSI display uses Windows-1252; raw bytes remain authoritative when the producer
uses another encoding. Console control characters are escaped. Invalid UTF-16
surrogates display as replacement characters while their raw bytes remain intact.
DBWIN scans at most 4092 bytes after the PID. Unreadable buffers and capture bounds
are reported explicitly without executing MMIO reads or modifying guest arguments.
The capture bounds are 16 MiB per text value and 4096 arguments or 64 MiB per call.

## Matched DLL evidence

| Guest file | SHA-256 | Observed implementation |
|---|---|---|
| System32/kernelbase.dll | cafc6d245c7c6fe94556bb799475f02af22325a5e7c4d229b93f2d5e7485fc3c | A RVA 0x253C0; W RVA 0x324B0 |
| SysWOW64/kernelbase.dll | 7d548e045c914efe5f6086a0fb86a1c2c71eef56fa0429afc7ed165218f0a3e0 | A RVA 0x105B20; W RVA 0x1D6FF0 |
| System32/ntdll.dll | e1ff52ee55a24dc03923f37c325d161e43626f5d43a58d5e16488e998202ed91 | DbgPrint RVA 0x51AC0; DbgPrintEx 0x51450; internal formatter 0x51B08; DebugPrint INT 2D 0xA11C2 |
| SysWOW64/ntdll.dll | 9dbf249b38dc2eded87b5d92b5e4035a2480f478586c9b3986dc4574f22cc5cb | Internal formatter RVA 0x2BB31 calls NtWow64DebuggerCall |

Both kernelbase versions pass exception 0x40010006 with count/pointer and
0x4001000A with UTF-16 count/pointer plus ANSI count/pointer. Counts include the
terminator. The x64 ntdll INT 2D helper passes raw buffer RCX, USHORT length DX,
component R8D and level R9D. The ReactOS x86 INT 2D EBX/EDI convention is distinct
from this modern x86 DLL's WoW64 transport.

References: Windows SDK 10.0.26100.0 `um/debugapi.h:50-63` and
`shared/ntstatus.h:1390,1426`; MSVC 14.44.35207 `include/vadefs.h:105-110,153-161`;
the separately recorded ReactOS debug-print source audit. Reference files were
read only. DLL hashes were checked against the guest root and selected IDBs.

## Persistent files

Each game capture contains `printed.log`, lossless `printed-events.jsonl`,
`printed-status.json`, and the restart index `printed-index.json`. Repeated calls
are separate evidence. The journal preserves source capture, line byte offsets,
line SHA-256, module/RVA and inherited history. The live panel shows a bounded
preview beneath Last: Suspicious; full text remains in the files.

Validation: 32 native tests passed in interpreter mode and 32 in JIT mode,
including actual INT 2D execution, x86 argument layouts, Unicode/ANSI fallback,
embedded NULs, MMIO rejection, DBWIN bounds and unchanged capture-failure status.
Twelve journal tests and the printed/checkpoint PowerShell panel checks passed.
These checks do not establish that Destiny reaches its menus or world.
