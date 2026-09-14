# File wait-completion targets

`NtAssociateWaitCompletionPacket` rejected file handles with
`STATUS_OBJECT_TYPE_MISMATCH`. Include existing file handles in target validation
and signal inspection. Retain the file while the packet is associated; release
that reference on dequeue or cancellation. A queued packet is delivered once.

Windows API probes accept regular files, overlapped files and directories after
open, report `AlreadySignaled = TRUE`, and return the supplied completion data.
The emulator uses its existing synchronous file-I/O signal model.

Four regression cases cover file/directory targets, one completion per
association, retained handle lifetime, cancellation and invalid handles. All
four fail on the previous handler; they pass with the change in interpreter and
JIT modes. The full focused set passes 29 tests in each mode.

Pending directory I/O now supplies the file signal state described in
[directory-notifications.md](directory-notifications.md). A newly opened file
is signaled; a pending notification clears that signal until completion.

Reference: https://learn.microsoft.com/en-us/windows/win32/devnotes/ntassociatewaitcompletionpacket

An actual application snapshot reproduced the old status `0xC0000024`.
The updated handler returns success from identical entry registers and sampled
memory. The saved return address is unchanged. This verifies the file-target
association, not a complete application boot or pending directory notifications.
