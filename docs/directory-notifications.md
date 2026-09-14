# Pending directory notifications

The notification stub returned success without issuing I/O. File waits treated every valid directory as signaled. A certificate-store worker consequently rearmed the same request and immediately received another wait-completion packet.

On Windows hosts, submit asynchronous directory I/O against the translated guest path with the requested filter, recursion flag and buffer size. Native requests use separate host buffers, status blocks and events. Completion copies the result into guest memory, signals the file or guest event, and queues a guest APC when requested. Pending requests keep file waits unsignaled. CancelIo requests and final file-reference cleanup release the native operations.

Serialize request arguments, signal state and already completed notification data. Re-arm pending requests before resuming a restored guest. The trailing snapshot extension permits loading earlier snapshots that have no directory-notification state. Changes to host files while a saved guest is not running are outside the recorded history. Other host platforms currently return STATUS_NOT_SUPPORTED.

Eleven notification regressions cover pending file waits, single packet delivery, event lifetime, filename data, short and zero buffers, cancellation, filtering, generic-read access, reference cleanup, snapshot restoration, invalid output ranges and 32-bit status-block layout. The focused suite passes 64 tests in interpreter and JIT modes. Native Windows probes provide the reference results for pending waits and change completion.

The same application-entry snapshot returns immediate success and AlreadySignaled=1 before the change, then STATUS_PENDING and AlreadySignaled=0 afterward. Entry registers and sampled memory match; both caller return addresses and stack pointers are preserved. A subsequent capture contains no further directory rearm calls and reaches application file reads, hashing and thread setup. Full application boot remains a separate acceptance check.

References:
- https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw
- https://learn.microsoft.com/en-us/previous-versions/mt812581(v=vs.85)
- https://learn.microsoft.com/en-us/windows/win32/devnotes/ntassociatewaitcompletionpacket
