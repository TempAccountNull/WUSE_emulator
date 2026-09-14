# CMApi class registry keys

DeviceApi\CMApi previously returned STATUS_SUCCESS without writing its output.
In the captured Windows build, cfgmgr32!Local_CM_Open_Class_Key reads a status
and handle from that output. It therefore returned old stack data as a registry
handle. devobj!DevObjGetClassDevs increments the index on ERROR_INVALID_HANDLE
and only ends its registry loop on ERROR_NO_MORE_ITEMS.

The CMApi device now implements class-key IOCTL 0x470863 against the guest
registry. Type 2 selects Control\Class; type 3 selects Control\DeviceClasses.
A null name opens the root; a GUID selects its class subkey. OpenAlways creates
a missing class in the guest overlay. No host registry API is used.

The 48-byte x64 request and 36-byte WoW64 request are decoded separately.
The 16-byte output contains size, operation NTSTATUS and a 64-bit guest handle.
IO_STATUS_BLOCK.Information is 16 on transport success, including an operation
failure. Failure outputs contain a zero handle. A failed output write closes a
newly opened handle. Other IOCTLs return STATUS_NOT_SUPPORTED.

## Protocol references

The following IDA inputs were verified against the guest-root binaries:

| Module | SHA-256 | Function / RVA |
| --- | --- | --- |
| cfgmgr32.dll | 7fefd329b398827d6babf33973eaa2c0dc580fe1f3b291efa4793e795e4df8fa | InitializeInputRegistryData / 0x10000; Local_CM_Open_Class_Key / 0x10090; CM_Open_Class_KeyW / 0x101F0 |
| ntoskrnl.exe | c3c0dc9599964633d8958558e79051e50278cbca998a4ce2b5b769134b762eda | PiCMOpenClassKey / 0x622BDC; PiCMCaptureRegistryInputData / 0x628CD8; PiCMReturnHandleResultData / 0x628A10 |
| devobj.dll | 1b96bcec045e48833b5eec1a86e675608efd0683ab7a73459a26b7189acecacb | DevObjGetClassDevs / 0x44C0; RegEnumKeyExW call / 0x4CCC |

Public behavior: https://learn.microsoft.com/en-us/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_open_class_keyw

## Validation

Eight regression cases cover usable enumeration handles, both class roots,
missing keys, guest-overlay creation, WoW64 decoding, malformed requests,
unsupported operations, invalid pointers and handle cleanup.

The actual game restores the same CM_Open_Class_KeyW entry checkpoint for both
builds. Before the fix, the 16 output bytes remain unchanged and the caller
returns stale handle 0x04B00CC0. After the fix, the output carries guest registry
handle 0x05000084 and reports 16 bytes. The original return to
cfgmgr32.dll+0x1F1E8 is preserved. Further execution passes the old retry loop
and reaches the unimplemented graphics syscall NtGdiDdDDISetQueuedLimit.
Full Sunrise boot remains unconfirmed.

The class-key handler uses the existing guest registry access model; it does
not add registry ACL enforcement, device-query RPC support or other CMApi
operations. Those are separate functionality.
