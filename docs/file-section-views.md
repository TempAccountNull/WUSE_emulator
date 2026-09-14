# File-backed section view sizes

NtMapViewOfSection now maps the requested file range. A zero ViewSize maps
the remaining section; nonzero sizes round up to a page. SectionOffset
rounds down to allocation granularity. Explicit maximum section sizes
bound the view. Invalid ranges fail before allocation.

File data is read in 64 KiB chunks directly into the view. The old path
read the entire file into a temporary vector and mapped every byte after
the offset, regardless of ViewSize. Failed reads or writes release the
partially constructed view.

## Actual-game evidence

The cursor-position-supported run stopped in NtMapViewOfSection at
ntdll.dll+0x9DB54 with "Failed to write memory". The section referred to
destiny2.exe, a 122,984,224-byte file. ViewSize was 0xA00000 (10 MiB),
SectionOffset was zero, and the output base remained zero.

The guest-root imagehlp.dll database identifies the nested return as
MapIt2+0x10A, imagehlp.dll+0x7956. Its caller was
ImageGetCertificateDataEx, return imagehlp.dll+0xA76D. MapIt2 explicitly
limits its initial view to 10 MiB.

The corrected build restored the earlier guest checkpoint without
patching guest instructions, registers or data. In file-view-supported,
map-entry-100 and map-entry-102 record that same initial mapping request;
execution continued beyond both, past the previous failing call. The
file-view-verified continuation subsequently mapped later file ranges
and other DLLs during certificate processing. These captures establish
progress past the old stop; they do not provide a direct before/after
byte comparison for that initial 10 MiB call.

A later MapIt2 call in file-view-verified maps the guest-root ntoskrnl.exe.
NtMapViewOfSection returns STATUS_SUCCESS, ViewSize 0xA00000 and base
0xBAAA0D0000. Its return reaches kernelbase.dll+0x47F66 with RSP advanced
by eight bytes. Three 4 KiB samples at offsets 0, 0x500000 and 0x9FF000
match the original guest-root file. The measured syscall interval,
including debugger observation overhead, is 0.113 seconds.

The initial probe compared this later mapping against destiny2.exe and
stopped on a source-file mismatch. The raw file-view-proof.json preserves
that failed comparison. file-view-source-verification.json records the
correct source, its SHA-256, the preceding NtCreateSection log evidence,
and matching sample hashes. The guest then resumes from the saved return
point in file-view-return-continued. This is a probe correction, not a
guest mapping failure or evidence that every byte was compared.

All actual-game runs use Icicle JIT, explicit -e and -v, and the existing
game directory mapping. Sunrise boot is not yet confirmed.

## Validation

Release and tidy builds pass. All 23 selected tests pass in interpreter
and JIT modes. Five new file-view tests cover requested sizes, copied
bytes, page rounding, zero-filled tails, offset rounding, section limits,
invalid ranges, and committed-memory accounting after unmap.

The guest CLI smoke completes in 25.67 seconds with 29/30 categories
passing. The APIs category still fails.

## Remaining scope

The view-size bug is demonstrated independently of the underlying
backend write failure. The physical allocator's cause of failure has
not been established by an error code or allocator count. This change
does not establish that long-running backend memory growth is solved.

File-backed views retain the existing copy-based implementation. This
change does not add writable-view coherence, fixed-base placement or
per-view protection validation.

Reference: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwmapviewofsection
