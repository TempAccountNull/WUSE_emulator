# CMApi device-interface enumeration

IOCTL 0x470807 supplies CM_Get_Device_Interface_ListW and its size query.
It previously returned STATUS_NOT_SUPPORTED. The handler now decodes the
40-byte x64 or 36-byte WoW64 input and writes the buffer-result protocol.

ALL_DEVICES enumerates interface registrations from the guest SYSTEM hive and
registry overlay. It includes named references, optional case-insensitive
DeviceInstance filtering and a terminated UTF-16 MULTI_SZ. Missing or empty
classes return a single UTF-16 terminator. Short buffers report the complete
required size with STATUS_BUFFER_TOO_SMALL in the result header and transport
success. Invalid request pointers, alignment, sizes and flags are checked.

PRESENT queries return no interfaces because Sogen has no PnP runtime interface
activation path. A saved registration alone does not make host hardware active
in the guest. This change does not implement device activation, device-query
RPC, default-interface preference or PnP access-control filtering.

The result header is 20 bytes; payload starts at offset 16. Information counts
20 plus payload bytes. The final four counted bytes are not additional payload
and remain untouched when the payload extends past the initial 20-byte header.

## References

Verified guest-root IDA inputs:

- cfgmgr32.dll: CM_Get_Device_Interface_ListW RVA 0x1D70;
  Local_CM_Get_Device_Interface_List_Size RVA 0x2DB4.
- ntoskrnl.exe: PiCMGetDeviceInterfaceList RVA 0x60B478;
  PiCMCaptureInterfaceListInputData RVA 0x60B65C;
  PiCMReturnBufferResultData RVA 0x62C594;
  IopGetDeviceInterfaces RVA 0x62F318;
  CmDeviceClassesSubkeyCallback RVA 0x7B1290;
  PiControlGetDeviceInterfaceEnabled RVA 0x62A270.

https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iogetdeviceinterfaces

## Validation

The same actual-game checkpoint was replayed before cfgmgr32.dll+0x1ECA.
The old call returned C00000BB. The corrected call returned STATUS_SUCCESS,
Win32 TRUE and 22 bytes: size 20, operation status 0, required bytes 2, reserved
0, followed by an empty list. The NtDeviceIoControlFile return to
kernelbase.dll+0x2DDDB and DeviceIoControl return to cfgmgr32.dll+0x1ED1 both
preserved their stack. The request used PRESENT for interface class
{00000001-5f60-4c4f-9c83-a7953298d40d}.

Evidence: cmapi-interface-ioctl-old/interface-ioctl-proof.json and
cmapi-interface-ioctl-fixed/interface-ioctl-proof.json in the local captures.
The continuation executes further interface queries successfully and reaches
the separate unsupported XRSTOR at ntdll.dll+0xA1BC9. Sunrise boot is unconfirmed.

Release and tidy builds passed 27 targeted tests in both interpreter and JIT
modes, including eight new interface-list cases. CLI smoke still fails the
existing Message Queue (Paint) test with C000041D.

## 2026-09-15 DLL and enumeration cross-check

Rechecked the guest-root cfgmgr32.dll IDBs: x64 CM_Get_Device_Interface_ListW
at RVA 0x1D70 and its size helper at 0x2DB4; x86 list function at 0xB930.
The inline GUID starts at input +8 in both layouts. Public ALL_DEVICES (1)
maps to native flags 0; PRESENT (0) maps to 0x10000. Both callers copy from
output +16 using the required-byte count at +8.

x64 SHA-256: 7fefd329b398827d6babf33973eaa2c0dc580fe1f3b291efa4793e795e4df8fa
x86 SHA-256: 8440d611230b121f525033b26d6774ec781ba4339b34ff82bb21f3f54b2c6eb0

The after-afd-address-list capture contains 391,339 queries in the measured
prefix. All 583 distinct class names exist in the guest SYSTEM hive. Of 671
observed cycles, 667 contain each class once; four include interleaved queries.
Pointer-shaped GUID names also occur verbatim in the hive. This rules out
using those names alone as evidence of request corruption. It does not prove
the final NtEnumerateKey status or explain why the caller repeats the sweep.

ReactOS dll/win32/setupapi/cfgmgr.c uses PNP RPC for this public API. Its
transport cannot substitute for the verified Windows 10 CMApi layout.
Local evidence: sogen-fixes/cfgmgr-crosscheck-20260915.json and
cmapi-current-{sample,cycle}-evidence.json. No handler change follows from
this cross-check. Dawn boot remains unconfirmed.
