# NtUserCallOneParam window and quit operations

NtUserCallOneParam now handles the window-mapping operation used by
user32!ValidateHwnd. It returns the existing guest USER_WINDOW allocation.
The window must exist and its handle generation, type, destruction flag and
handle-table pointer must match. The 0xffff generation accepted by the guest
validation helper is supported. The operation leaves the object and client
thread data unchanged. Unknown selectors still stop with their actual value.

The selector is obtained from the loaded x64 user32 GetParent wrapper's call to
ValidateHwnd. The bounded helper pattern must call the loaded win32u export
through an IAT slot inside user32. This avoids assigning a fixed meaning to
0x38 across Windows versions. Unrecognized wrapper layouts remain unsupported.
The observed operation is a window mapping; other USER object operations are
not inferred from it.

## Evidence

Guest user32.dll SHA-256:
00db6f02d4a36226e13883b651f470d8cccd552ef5a79672b803f3fa76e4b3d5

IDA verified GetParent at RVA 0xF1E0 calls ValidateHwnd at RVA 0xF2A0.
The helper loads EDX=0x38 and calls NtUserCallOneParam through RVA 0x91018.
It uses the returned pointer as a tagWND, then GetParent examines the parent
or owner fields. No host GetParent call replaces this guest code.

The saved game stop contains HWND 0x06800003. The original return slot is:

```text
RSP:    0x1019C9D38
Bytes:  57 F3 10 07 01 00 00 00
Return: 0x10710F357 / user32.dll+0xF357
```

The old handler returned STATUS_NOT_SUPPORTED at win32u.dll+0x1084.
That snapshot also contains independently reproduced read-only zero-page
corruption. Its handle entry is not a valid window pointer; returning that
value as success would hide the fault. Actual boot verification uses a fresh
guest run with the page-isolation fix. See icicle-readonly-pages.md.

PostQuitMessage is also dispatched through the selector read from its guest
wrapper (0x3B in this DLL). It calls the existing guest message-queue handler.
The CLI smoke passes the former unsupported PostQuitMessage stop and later
ends with its separate Window Geometry C000041D exception.

Six tests cover mapping, invalid/deleted objects, the wildcard generation,
import/selector validation, full snapshot restore and quit-message delivery
to the calling thread. Release and tidy builds pass; each startup suite
passes 97 cases in both interpreter and JIT modes. This is regression
evidence, not confirmation that Sunrise has booted.

Evidence is retained in PatchScanner/benchmarks/sogen-fixes and in
destiny-sogen-1789382772809802000-window-mapping-control.

PostQuitMessage reference:
https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-postquitmessage
Guest bytes at user32.dll+0x2C490:

```text
48 63 C9                movsxd rcx, ecx
BA 3B 00 00 00          mov edx, 0x3B
48 FF 25 79 4B 06 00    jmp [NtUserCallOneParam]
```

Final release suite: regression-1789384022930996600.
Final tidy suite: regression-1789384176964305400.
CLI smoke: sogen-cli-smoke-1789384021936537800.
