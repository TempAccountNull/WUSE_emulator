# GDI handle-table pointer ABI

Legacy GDI DLLs read GDI_HANDLE_ENTRY.UserPointer directly. Sogen previously
encoded every pointer with the PEB cookie, causing SelectObjectImpl to
dereference a noncanonical address on the Windows 10 guest.

Pointer encoding now requires the guest win32u.dll export NtGdiInit2.
Without it, user pointers stay raw and GdiDCAttributeList is left unchanged.
NtGdiInit returns TRUE; NtGdiInit2 returns the initialized cookie. The existing
32-bit and 64-bit rotations remain in use for the cookie ABI.

## Guest evidence

The guest reports 10.0.19045.7417. Its gdi32.dll and gdi32full.dll are
10.0.19041.7417. win32u.dll exports NtGdiInit at RVA 0x6C90 and has no
NtGdiInit2 export.

- gdi32full.dll initialization calls NtGdiInit through IAT RVA 0xB2E30
  and compares its return against 1 at RVA 0x22A03.
- gdi32.dll!GdiGetEntry, RVA 0x26B0, copies the 24-byte handle entry without
  decoding UserPointer.
- gdi32full.dll!SelectObjectImpl loads that pointer, then executes
  `4C 8B B7 28 01 00 00` / `mov r14,[rdi+0x128]` at RVA 0x1BFF8.

The newer cookie ABI is also described in the author's GDI investigation:
https://medium.com/@TTodlost/wow64-madness-running-native-x64-inside-a-wow64-process-and-everything-that-breaks-e8ce2087be66

## Actual-game replay

Both builds resumed the same checkpoint, SHA-256
`00becb9869dc4cffc06d201322130532f6756c073d335cd43c1fc6951fa5635a`.
The capture client checked the instruction bytes and read the table entry
from guest memory; no guest pointer or register was patched.

| Observation | Old build | Corrected build |
| --- | --- | --- |
| HDC | 0x02012026 | 0x02012026 |
| Object | 0x270000 | 0x270000 |
| UserPointer | 0x8000000000138000 | 0x270000 |
| Instruction | gdi32full.dll+0x1BFF8 | gdi32full.dll+0x1BFF8 |
| Single step | C0000005, read 0x8000000000138128 | RIP advances to gdi32full.dll+0x1BFFF |
| Function return | Fault before return | gdi32.dll+0x36B5; saved return slot consumed correctly |

Local captures: `gdi-pointer-old-observed` and `gdi-pointer-supported`, each
with gdi-pointer-proof.json, register snapshots, verbose log and a checkpoint.
DLL hashes and the original exception record are in
`graphics-exception-entry/gdi-fault-analysis.json`.

This fixes the MessageBox drawing path after a reported graphics-device
failure. It does not resolve that preceding device failure. Sunrise boot is
unconfirmed. Existing encoded objects in checkpoints from old builds are
not rewritten; this replay creates the affected DC after restoring state.

## Validation

Release and tidy builds passed 48 selected tests in each of interpreter and
JIT modes. Seven GDI tests cover legacy initialization, DC/brush pointers,
an old PEB cookie, cookie initialization and return, both pointer widths,
and handle reuse. Cookie-mode validation uses an isolated export fixture;
it is not a modern Windows guest boot test.

The CLI smoke test advances beyond its previous C000041D failure, then stops
at unsupported NtUserCallHwndLock operation 0x73 in UpdateWindow.
