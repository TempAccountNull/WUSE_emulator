# Isolate host writes to read-only zero pages

Icicle maps untouched read-only memory to a shared zero page whose
copy_on_write flag is false. Sogen's host-side memory writes use perm::NONE
to initialize guest data without granting the guest write access. After a
read materializes the shared zero page, such a write modified every virtual
mapping of that page. Server information, USER handles and window objects
could overwrite one another.

The bridge now marks shared zero pages copy-on-write before a host write and
invalidates their cached write translation. Icicle detaches the addressed
page during the write. Guest permissions are unchanged; untouched pages stay
shared. Address overflow is rejected before writing.

Upstream dependency inspected: icicle-emu revision
3292602fd4857b53ed653dec69398efe4cdb435e,
icicle-mem/src/physical.rs (PhysicalMemory::new) and
icicle-mem/src/mmu.rs (write_physical).

## Reproduction

Map two read-only allocations, read both, then write 0x123456789abcdef0 to
the first through the host API. Previously the same value appeared in the
second. A write across a page boundary also changed the unrelated allocation.
Both interpreter and JIT reproduce this. Both pass with the bridge fix.

The real game checkpoint had the same object bytes at 0x101430000,
0x101450000 and 0x101630000. The USER entry at 0x101450060 held
0x43800000780 (1920 and 1080 packed together) instead of an object pointer.
The original snapshots are evidence of that corruption; this fix does not
repair their already-modified contents. Fresh guest startup is required.

Control test: PatchScanner/benchmarks/sogen-fixes/
regression-1789383104827072500 (one failed case in each mode).
Corrected startup suite: regression-1789383234301428500
(96 passed in each mode).

The fresh Destiny checkpoint has separate source-identified allocations:
USER_SERVERINFO at 0x101430000, USER_DISPINFO at 0x101440000 and the handle
table at 0x101450000. Count 65534, display metrics, font and DPI fields match
their source definitions. The server/display/table prefixes are distinct.
Read-only streaming snapshot verification is retained as
PatchScanner/benchmarks/sogen-fixes/fresh-user-memory-proof.json.

The 66 obsolete completed snapshots were removed at the user's request.
Their SHA-256 inventory and compact failure observations remain available.
The fresh baseline and its continuations replace them for boot acceptance.
