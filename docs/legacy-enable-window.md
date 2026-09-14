# Legacy EnableWindow dispatch

NtUserCallHwndParamLockSafe is now registered. Its EnableWindow operation is
resolved from the guest wrapper and verified import target, then dispatched
to the existing NtUserEnableWindow handler. Unknown selectors still stop
with an unsupported-operation diagnostic.

The verified guest user32.dll wrapper is at RVA 0x2BC70. It sign-extends EDX,
sets R8D=0x7D, and jumps through IAT RVA 0x90FA0 to
win32u.dll!NtUserCallHwndParamLockSafe. InternalDialogBox makes the same call
at user32.dll+0x2DFE7 to disable its owner before creating the modal dialog.

## Actual-game validation

The original run stopped at win32u.dll+0x8564 with C00000BB.
The corrected run used HWND 0x06800007 and parameter 0:

- Guest USER_WINDOW style changed from 0x10CB0000 to 0x18CB0000.
- The syscall returned 0 because the window was previously enabled.
- The return address was user32.dll+0x2DFEE; RSP advanced by eight bytes.
- The same run continued through subsequent DLL loads and device enumeration.

Local captures: `legacy-dialog-supported-continued` contains the original
stop, and `enable-window-supported/enable-window-proof.json` contains the
USER handle-table entry, window structures, registers and return validation.
The input checkpoint was the verified MessageBeep return. No guest pointer,
instruction or register was patched.

## Validation and scope

Release and tidy pass 30 selected tests in both interpreter and JIT modes.
Three added tests cover selector/import validation, enable/disable state
transitions and return values, invalid HWNDs, and unknown-operation stops.
The existing snapshot test also covers this dispatch.

The CLI smoke run completes with 29 of 30 categories passing. The remaining
failure is APIs; Paint and GDI pass. Sunrise boot remains unconfirmed.

This change retains the existing handler's state and UI behavior. It does
not add WM_CANCELMODE/WM_ENABLE callback delivery, which the native API also
requires. That behavior remains a separate compatibility gap.

https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enablewindow
