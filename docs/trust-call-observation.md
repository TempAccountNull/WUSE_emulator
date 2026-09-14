# Preserve guest state when tracing WinVerifyTrust

The function-detail collector changed RIP to the saved return address, advanced RSP by eight bytes, and cleared RAX when it observed WinVerifyTrust. Icicle continued executing the translated DLL body. Its RET then consumed caller data above the original return slot. The function event also reported the caller address as the DLL entry.

Remove those register writes. Logging observes the call; the guest DLL executes and returns its own result through the original return slot.

AnalysisObservation tests compare the complete saved register file across the logging callback and execute a traced CALL/body/RET sequence. Both tests fail before the change and pass afterward in interpreter and JIT modes. The focused regression set passes 53 tests in each mode. Release and tidy builds pass. The CLI sample retains its existing C000041D Window Geometry failure.

A replay of the same application-entry snapshot reproduces the eight-byte stack shift on the old analyzer. The corrected analyzer preserves the return slot, returns to the caller with 0x800B0100, and continues executing application code. Full application startup remains a separate acceptance check.
