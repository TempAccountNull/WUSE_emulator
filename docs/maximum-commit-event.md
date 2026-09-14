# MaximumCommitCondition event

NtOpenEvent and NtCreateEvent now resolve the kernel-owned
\KernelObjects\MaximumCommitCondition notification event. It starts
nonsignaled, retains a kernel reference after the last user close, and uses
the existing query, wait and snapshot behavior. OBJ_CASE_INSENSITIVE is
honored when looking up named events. Creating an existing kernel event
does not replace its type or initial state.

## Reference

The guest kernel's MiCreateMemoryEvent (ntoskrnl.exe+0x7A0A00) calls
ZwCreateEvent with NotificationEvent and InitialState=0, then retains a
handle and an object reference. Microsoft documents this event as a
notification of commit charge approaching the maximum commit limit.

https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/standard-event-objects

Sogen does not currently model system-wide commit-pressure transitions.
This change provides the event object and normal nonsignaled state; it does
not derive guest pressure from the host's memory usage. General per-handle
access-right enforcement is unchanged.

## Actual-game validation

Two Icicle JIT runs restored the same Destiny checkpoint with SHA-256
255bb6506adfb60c231b0963ed987f8c341356a849d81281404f6e7d88c15545.
Both used explicit -e, -v and the existing game-directory mapping.

clbcatq.dll+0x17D34 calls NtOpenEvent with access 0x100001. Before the fix:
status C0000225, handle 0, helper return 0, DllMain return FALSE, and
g_fDllInitialized=0. Its separate s_fFailLoad flag was zero.

After the fix: status 0, handle 0x1800080, helper returned that handle,
DllMain returned TRUE and g_fDllInitialized became 1. The syscall returned
to clbcatq.dll+0x17D3B with RSP advanced by 8; the helper returned to
+0x1ED41 and DllMain to +0x1DACF. No guest instructions, registers or
pointers were patched. The same process continued until an unsupported
NtUserCallTwoParam call through user32!GetCursorPos.

Local proof files are maximum-commit-before/maximum-commit-proof.json and
maximum-commit-supported/maximum-commit-proof.json in the capture directories.

## Validation

Release and tidy builds pass. All 22 selected tests pass in interpreter and
JIT modes: six new event tests, directory notifications, file wait
completion and section-first-execution snapshot restore.

The guest CLI smoke completes in 28.78 seconds with 29/30 categories
passing. APIs still fails. This is separate from Sunrise boot acceptance,
which remains unconfirmed.
