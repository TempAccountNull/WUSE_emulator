# Legacy USER dialog dispatch

The Windows 10 guest calls MessageBeep through NtUserCallOneParam and
UpdateWindow through NtUserCallHwndLock. These paths previously stopped as
unimplemented operations even though Sogen has handlers for both functions.

Dispatch now resolves the operation from the guest user32.dll wrapper and
verifies its mapped win32u.dll import target. It does not hardcode selectors
across guest versions. Unrecognized wrapper layouts and targets remain
unsupported. Snapshot restoration works without discarded import metadata.

UpdateWindow uses the existing pending-paint callback and completion path.
MessageBeep logs its requested type and uses the existing silent handler;
this change does not implement registry-selected sound playback.

## Verified guest wrappers

IDA input: root/filesys/c/windows/system32/user32.dll, image base 0x180000000.

- MessageBeep RVA 0x86590: zero-extend ECX, set EDX=0x39, then jump through
  NtUserCallOneParam IAT RVA 0x91018.
- UpdateWindow RVA 0x22E0: validate HWND; the slow path sets EDX=0x73 at
  RVA 0x2316 and jumps through NtUserCallHwndLock IAT RVA 0x91040.

The actual game stopped in SoftModalMessageBox at user32.dll+0x79B27,
requesting MessageBeep(0x10). The corrected replay returns TRUE at
win32u.dll+0x1084, then returns to user32.dll+0x79B2E with the saved
return address consumed correctly. No guest instruction, register or pointer
was patched to skip the call.

Local evidence: `gdi-pointer-supported-continued` contains the original
unsupported-operation stop; `message-beep-supported/message-beep-proof.json`
contains before/after registers and the return check.

## Validation

Release and tidy each pass 27 selected tests in both interpreter and JIT
modes. Six new cases cover selector changes, mismatched imports, dispatch,
empty and pending update regions, and snapshot restoration.

The CLI smoke test now completes all 30 categories: 29 succeed, including
Message Queue (Paint) and GDI. The APIs category fails and the guest exits 1.
This is separate from actual game boot acceptance, which remains unconfirmed.

API contracts:
https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-updatewindow
https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-messagebeep
