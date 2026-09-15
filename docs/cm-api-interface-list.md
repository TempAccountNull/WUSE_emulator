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

## Actual-game end-of-enumeration observation

The fresh CNG-validation game repeated the same 583 PRESENT classes. Its last
16 MiB contained 1,834 successful interface queries, each class seen three or
four times. A checkpoint from that run was resumed without changing guest
results or crypto state, with breakpoints at the enumeration-exhausted branch
and DevObjGetClassDevs entry/return in the matched devobj.dll.

Three observed sweeps ended with ERROR_NO_MORE_ITEMS at index 583 at
devobj.dll+0x4D07. DevObjGetClassDevs then returned TRUE to
setupapi.dll+0x2EBF on thread 0x54. The next invocations used flags 0x16.
The end-of-enumeration result is therefore present and consumed correctly;
the caller restarts the sweep. This does not establish that the polling causes
the white window. The caller above setupapi remains under investigation.

ReactOS SetupDiGetClassDevsExW rejects a null interface class even with
DIGCF_ALLCLASSES. The matched Windows build permits the observed all-class
enumeration; the ReactOS branch cannot be substituted for this DLL's behavior.

Exact boundary registers, stacks, binary hashes and checkpoint ancestry are
in cm-api-enumeration-boundaries.json. No CMApi behavior was changed.

## Matched SetupAPI callers

The x64 and x86 guest-root setupapi.dll hashes match the inspected IDBs.
SetupDiGetClassDevsExW is at RVA 0x2DB0 (x64) and 0x194A0 (x86). Both
validate Reserved and flags, create or access the device set, and call
DevObjGetClassDevs once. On failure they destroy a new set or release an
existing set and preserve the error. Neither contains an outer retry loop.

Both permit a null class with DIGCF_ALLCLASSES, reject nonzero Reserved
with error 87, and reject (Flags & 0x11) == 1 with error 1004. The observed
internal set fields differ: hwnd/reference-count/mutex offsets are
8/328/392 on x64 and 4/188/220 on x86. These offsets describe these builds,
not a portable public structure.

The captured x64 frame unwinds to setupapi.dll+0x2D67 inside
SetupDiGetClassDevsW. That wrapper calls ExW once. The higher caller is
outside the captured stack range and has not been identified by this sample.

Wine dlls/setupapi/devinst.c:2106 supports the all-interface-class sweep in
a finite CM_Enumerate_Classes loop. Its unsupported flag and remote-machine
paths prevent using it as a complete substitute. ReactOS's null-interface
class rejection differs from the matched DLL. These sources were read only;
no reference tree was built or executed. Full disassembly and hash evidence
are saved in cm-api-enumeration-boundaries.json.
