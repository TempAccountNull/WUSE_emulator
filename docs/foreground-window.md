# Foreground window activation

The captured Windows x64 `user32!SetForegroundWindow` wrapper loads operation
`0x70` into EDX and jumps through its `NtUserCallHwndLock` import. The guest
`win32u.dll` stub uses syscall `0x1021`. Sogen previously had no handler for that
syscall; the separate `NtUserSetForegroundWindow` stub returned zero without
changing window state.

`NtUserCallHwndLock` now recognizes the operation from the loaded guest wrapper
and verifies its resolved IAT destination against the `win32u` export. It does
not assume that the selector or syscall number is constant across Windows
versions. Other selectors and unrecognized wrapper layouts retain an explicit
unsupported-operation stop.

The lookup reads guest memory because saved module metadata omits import names.
A first replay exposed that difference; the regression now tests lookup after
a full snapshot restore, including rejection of a mismatched IAT destination.

Activation publishes the selected guest window in process state and SERVERINFO,
delivers activation messages through existing guest callbacks for the calling
thread, and queues messages to other owning threads. Repeated activation does
not repeat messages. Disabled, ordinary child, message-only, and invalid windows
are rejected. A minimized window receives the minimized activation bit without
a focus message. Callback state is serialized, and a callback that changes the
foreground window prevents the outer transition from continuing to send stale
messages. The guest desktop is excluded from application activation broadcasts.

This uses Sogen's existing foreground/focus model. It does not implement attached
input queues, foreground-lock arbitration between processes, host focus stealing,
priority boosting, or a complete Windows z-order manager. Guest window procedures
execute in the emulator.

## Captured modules

| Module | SHA-256 | Relevant RVA |
| --- | --- | --- |
| win32u.dll | 279f06b781954473bcf30626fee3d58a25a8c5cc58456145adfb9599b0551cc9 | 0x1450: Nt/ZwUserCallHwndLock, syscall 0x1021 |
| user32.dll | 00db6f02d4a36226e13883b651f470d8cccd552ef5a79672b803f3fa76e4b3d5 | 0x2C850: SetForegroundWindow, selector 0x70 |

IDA instances 13341 and 13345 were checked against the guest-root files.
`NtGdiCreateRectRgn` is a different stub at win32u+0x2050, syscall 0x1081.
This user32 build's GetForegroundWindow export at RVA 0x34C00 jumps to
NtUserGetForegroundWindow; it does not directly read SERVERINFO.

## Validation

Eight WindowActivationTest cases cover shared state, message routing, repeat
activation, invalid windows, previous-window deactivation, minimized state,
snapshot restoration, callback interruption, and wrapper/IAT resolution.
Release and final clang-tidy builds passed. The selected regression suite passed
77 tests in each of Icicle interpreter and JIT modes. The broader CLI sample
still reports existing API/MonitorInfo failures and a WindowGeometry exception.

The actual game checkpoint returned TRUE from NtUserCallHwndLock, preserved the
stack return slot, and executed RET to destiny2.exe+0x3C1413 with RSP advanced by
eight bytes. SERVERINFO.foregroundWindow changed from the guest desktop to the
requested window, and guest window callbacks completed. A subsequent bounded
run recorded 150,000 function/syscall events beyond this stop. It exposed a
separate invalid registry-handle retry loop. Sunrise boot remains unconfirmed.

## References

- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow
- https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-activate
