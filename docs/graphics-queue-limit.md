# Graphics present queue limit

NtGdiDdDDISetQueuedLimit now implements SET_PRESENT (1) and GET_PRESENT (2)
for the existing emulated device handle 0x5000. The limit is stored in process
graphics state. Zero resets it to 3. GET writes only the four-byte limit field.
Invalid handles and types return STATUS_INVALID_PARAMETER. Creating the device
resets the default. Snapshots preserve the setting; older snapshots start at 3.

This extends the existing single-device graphics model. It does not add a
hardware presentation queue, asynchronous frame scheduling, multi-device handle
lifetime tracking or flip-queue operations. Render currently reports zero queued
buffers. Queue configuration is observable through GET; it is not a rendering
completion claim.

## References

- Microsoft D3DKMT_SETQUEUEDLIMIT structure and default:
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_setqueuedlimit
- Windows SDK 10.0.26100.0, shared/d3dkmthk.h: SET_PRESENT=1, GET_PRESENT=2,
  hDevice at +0, Type at +4, QueuedPresentLimit at +8, total structure size 16.

## Actual game evidence

d3d11.dll SHA-256:
2359b84d482cc1b45966db45823aa6963bb58e16702ee3babc6b639597fa2539

The return site d3d11.dll+0x48B9E belongs to a shared CallAndLogImpl body.
Its displayed template name contains DESTROYSYNCHRONIZATIONOBJECT, but the
captured function pointer is win32u!NtGdiDdDDISetQueuedLimit (RVA 0x5930).
The log-name argument at d3d11.dll+0x1EA970 contains SetQueuedLimit.

The captured 16-byte request is:

```text
00 50 00 00 01 00 00 00 01 00 00 00 00 00 00 00
hDevice=0x5000, Type=1, QueuedPresentLimit=1
```

The candidate restores all 808 register bytes and sampled memory identically
to the entry capture. It returns STATUS_SUCCESS instead of the prior unsupported
syscall stop. The request and original return slot remain unchanged:

```text
RSP before: 0x1019CB178
Slot:       9E 8B 7B 0F 01 00 00 00
RET at:     0x100005944 / win32u.dll+0x5944
Return:     0x10F7B8B9E / d3d11.dll+0x48B9E
RSP after:  0x1019CB180
```

The continuation records 100,640 function/syscall calls before stopping at
NtUserCallOneParam, selector 0x38. The caller is user32!ValidateHwnd, reached
from GetParent. That operation remains unsupported; Sunrise boot is unconfirmed.

Five regression cases cover set/get with unchanged surrounding bytes, default
reset, invalid requests, device creation and full snapshot restore. The release
regression run passes 90 tests in both interpreter and JIT modes. The required
tidy build and broader CLI smoke results are recorded alongside the capture.

Evidence: PatchScanner/benchmarks/sogen-fixes/queued-limit-proof.json;
queued-limit-validation.json. Run totals include snapshot restore, save and
cleanup; they are not syscall latency measurements.
