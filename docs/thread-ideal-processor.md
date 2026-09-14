# Thread ideal processor

Application thread setup reached NtSetInformationThread class 13 and stopped with STATUS_NOT_SUPPORTED. Implement the processor preference using guest thread state. Return the previous processor index as the syscall result. Value 64 queries without changing the preference; inactive indices below 64 also leave it unchanged. Reject larger values, incorrect lengths, misalignment and inaccessible input with the observed Windows statuses. The input buffer remains unchanged.

NtQueryInformationThread class 33 returns the current PROCESSOR_NUMBER in group 0. Query class 13 returns STATUS_INVALID_INFO_CLASS. A trailing snapshot extension preserves preferences without changing the legacy thread layout, so earlier startup checkpoints remain loadable. The preference is advisory; this does not add parallel execution to the single-vCPU Icicle backend.

Native Windows probes on a disposable process verify previous-index returns, unchanged query/inactive-index behavior, invalid lengths, input alignment and query structure layout. Five regressions cover those results, target-thread isolation and full snapshot restoration. Actual-game replay evidence is recorded separately from these tests.

Reference: https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadidealprocessor

The actual application replay restores identical entry registers and sampled memory, returns the prior processor index 0, and reaches the original caller at kernelbase.dll+0x8BA4F with balanced stack state. Continued execution reaches device enumeration and SetForegroundWindow; the next stop is an unimplemented NtUserCallHwndLock. Release and tidy builds pass, with 69 focused tests passing in each interpreter and JIT mode. The CLI smoke retains its earlier Window Geometry C000041D failure. Full application boot is not yet confirmed.
