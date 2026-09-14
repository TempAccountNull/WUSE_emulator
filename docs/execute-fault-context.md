# Execute fault context

Instruction fetch faults must use operation 8 in the Windows exception record.
Operations 0 and 1 identify data reads and writes. See Microsoft's
[EXCEPTION_RECORD documentation](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record).

With instruction precision enabled, the last execution callback still names
the preceding instruction when fetching the next instruction fails. Preserve
the backend instruction pointer for execute access and guard-page faults.
Data faults retain their existing instruction-precision handling.

Icicle reports the instruction start for fetch violations. The bridge locates
the inaccessible byte within the maximum 15-byte x86 instruction, preserving
the instruction pointer separately. This handles instructions crossing into
non-executable pages and distinguishes unmapped targets.

Regression coverage runs with instruction precision both enabled and disabled:
RET and CALL into non-executable memory, data reads and writes, an instruction
crossing a protection boundary, an executable guard page, and an unmapped
return target. Check exception operation/address, saved RIP/RSP, and the return
address pushed by CALL. All 14 cases pass in interpreter and JIT modes.

An application replay from the same pre-RET snapshot confirms that only the
exception address, execute operation and saved RIP change. Other record and
context bytes match. The corrected exception remains unhandled at second
chance; this change does not repair the application's invalid return target.
