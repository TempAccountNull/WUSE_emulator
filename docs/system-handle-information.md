# Extended system handle information

NtQuerySystemInformation now handles SystemExtendedHandleInformation
(0x40). It enumerates Sogen's current guest process handle and live
entries in its NT handle stores. USER window/menu handles and pseudo
handles are not included. NtQueryObject returns matching type indices.

The native x64 result has a 16-byte header and 40-byte entries. Buffers
smaller than the header return STATUS_INFO_LENGTH_MISMATCH and a zero
return length. A partial buffer receives the total handle count and the
entries that fit; ReturnLength reports the full required size. A complete
buffer returns STATUS_SUCCESS.

## Reference

The guest-root ntoskrnl.exe database was inspected at:

- ExpGetHandleInformationEx: RVA 0x94A6C4
- ObGetHandleInformationEx: RVA 0x8DD090
- ObpCaptureHandleInformationEx: RVA 0x8DD1F0
- ExpSnapShotHandleTables: RVA 0x94CEF0

Those routines establish the header minimum, entry size, partial-buffer
behavior and required-length calculation. The field layout also matches
https://github.com/winsiderss/phnt/blob/master/ntexapi.h.

## Actual-game validation

file-view-return-continued stopped on class 0x40 with C00000BB at
0x105FCCD14, inside a copied syscall stub. The return address on its stack
was destiny2.exe+0x27F25B.

system-handles-supported restores the earlier guest checkpoint and
captures the same caller. Its first 4,096-byte buffer reports 264 handles,
10,576 required bytes and C0000004. The guest retries with 15,696 bytes;
the query returns 262 handles, 10,496 bytes and STATUS_SUCCESS. Both
returns reach destiny2.exe+0x27F25B with RSP advanced by eight bytes. The
handle inventory changes between queries.

system-handles-proof.json retains both buffers, every returned entry,
registers and return verification. The same process continues beyond
the query. Launches use Icicle JIT, explicit -e and -v. Sunrise boot has
not been confirmed.

## Validation and model limits

Release and tidy builds pass. All 16 selected tests pass in interpreter
and JIT modes. Five new cases cover partial buffers, adjacent-byte
preservation, handle lifecycle, type consistency, snapshot restoration,
invalid output and optional ReturnLength. The CLI smoke takes 23.79
seconds with 29/30 categories passing; APIs still fails. This smoke result is separate from actual-game evidence.

Entries describe the existing guest object model. Object values are
stable opaque identities in an unmapped guest kernel range, never host
pointers. Access is GENERIC_ALL and handle flags are zero, consistent
with the existing NtQueryObject handlers. Per-open access masks, distinct
duplicate-handle slots, kernel-held reference ownership and creation
backtraces are not added by this change. The implemented wire layout is
native x64; no WoW64 structure conversion is claimed.
