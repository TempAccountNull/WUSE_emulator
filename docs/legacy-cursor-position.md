# Legacy GetCursorPos dispatch

NtUserCallTwoParam is registered. The GetCursorPos operation is resolved
from the guest wrapper and verified import target, then dispatched to the
existing NtUserGetCursorPos handler. Other modes or selectors remain
explicit unsupported-operation stops.

The verified user32.dll wrapper at RVA 0x28450 is:

```asm
mov edx, 1
lea r8d, [rdx+0x7e]
jmp qword ptr [NtUserCallTwoParam]
```

The IAT slot is user32.dll+0x90D70. This build's selector is 0x7F; the
handler reads it from the wrapper instead of fixing that value across
Windows versions. Its target is win32u.dll+0x1570.

## Actual-game validation

The preceding run stopped at win32u.dll+0x1584 with C00000BB after
GetCursorPos reached the unregistered NtUserCallTwoParam syscall 0x102A.
R10 held the output pointer, RDX was 1 and R8 was 0x7F.

The corrected build restored the same earlier Destiny checkpoint without
patching guest code, registers or pointers. The syscall returned TRUE,
wrote POINT {822, 449}, preserved eight guard bytes on both sides, and
returned to destiny2.exe+0x342D57 with RSP advanced by eight bytes.
The same process continued through subsequent game instructions.

Local evidence: maximum-commit-supported contains the original stop;
cursor-position-supported/cursor-position-proof.json contains wrapper and
IAT bytes, before/after registers, POINT memory and return verification.
Both runs use Icicle JIT, explicit -e, -v and the existing game mapping.

## Validation and scope

Release and tidy builds pass. All 19 selected tests pass in interpreter
and JIT modes. Three new cases cover selector/import recognition, signed
screen coordinates, adjacent-byte preservation, null output, unsupported
modes and unknown selectors. The existing snapshot test now checks cursor
dispatch after import metadata has been discarded.

The CLI smoke completes in 25.85 seconds with 29/30 categories passing;
APIs still fails. Sunrise boot is not confirmed by this syscall test.

This change uses the existing single-desktop cursor state. It does not
add physical-coordinate variants, per-monitor DPI conversion or window
station access checks.

https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getcursorpos
