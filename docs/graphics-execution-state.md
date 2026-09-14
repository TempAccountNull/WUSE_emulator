# Graphics execution-state query

NtGdiDdDDIGetDeviceState returned zero for every query. Execution-state query
1 uses D3DKMT_DEVICEEXECUTION_STATE, where active is 1. D3D11 compares that
response with 1 and removes the device when it differs. The handler now returns
active for that query. Other query responses retain their existing behavior.

Reference: Windows SDK 10.0.26100.0 shared/d3dkmthk.h and
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ne-d3dkmthk-_d3dkmt_deviceexecution_state

The guest d3d11.dll function at RVA 0x27630 initializes an execution query and
returns its execution-state field. GetDeviceRemovedReason at RVA 0x48BD0
checks for active. Before this change, the actual game made 68 CreateTexture2D
calls returning DXGI_ERROR_DEVICE_REMOVED (0x887A0005), then passed a null
resource to Map.

## Validation

Both builds restored the identical actual-game checkpoint before win32u.dll
NtGdiDdDDIGetDeviceState. The old response was 0; the corrected response is 1.
The input header, remaining 44 bytes of the 56-byte query buffer and saved
return address were preserved. Both calls returned STATUS_SUCCESS to
d3d11.dll+0x2767E and restored RSP by eight bytes.

Evidence: graphics-execution-state-old/device-state-proof.json and
graphics-execution-state-fixed/device-state-proof.json in the local captures.
The corrected continuation reached a different failure after 394,504 traced
calls: unsupported XRSTOR at ntdll.dll+0xA1BC9. No successful Map or full Sunrise
boot is claimed from this run. CMApi IOCTL 0x470807 also remains unimplemented.

Release and tidy builds passed 23 targeted regressions in each interpreter and
JIT mode. Coverage includes execution/reset queries and a canary beyond the
scalar query prefix. The required CLI smoke still fails its existing Message
Queue (Paint) test with C000041D; it is not a passing whole-suite result.
