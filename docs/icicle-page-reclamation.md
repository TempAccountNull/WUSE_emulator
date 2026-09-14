# Icicle physical-page reclamation

Repeated guest map/write/unmap operations exhausted Icicle's fixed page pool.
Unmap removed virtual mappings but never returned their physical pages to the
allocator. A 16-page pool failed on the fifteenth write despite every previous
mapping having been removed.

The actual Destiny replay reproduced `MemError::OutOfMemory` with
`physical_pages=800000 capacity=800000`. `NtMapViewOfSection` failed after
105,906,176 of 122,984,224 bytes while mapping the complete on-disk executable.
ViewSize zero requests the remaining section; this request was valid.

## Change

Sogen collects physical-page candidates from removed mappings and reclaims
unreferenced pages at VM boundaries. At the normal pool capacity, reclamation
runs after 4,096 candidates accumulate, or sooner when the pool is nearly full.
Running JIT code and pending p-code replay defer reclamation. Existing code-cache
invalidation runs before slots can be reused.

Active aliases, shared zero pages and executed pages remain protected. Full
snapshots keep their copy-on-write data and matching pending-candidate list.
The memory API also accepts roots for separately retained virtual address spaces.
Reused slots reset their bytes, permissions and flags. The capacity is unchanged.

The local `icicle-mem` dependency retains upstream revision
`3292602fd4857b53ed653dec69398efe4cdb435e`, licenses and formatting configuration.
Its opt-in API leaves ordinary upstream unmap behavior unchanged. No shared Cargo
checkout is modified. `vendor/icicle-mem/UPSTREAM.md` identifies the local changes.

## Actual-game verification

Both runs restored the same pre-failure snapshot, SHA-256
`c920456cfe4a0f300b4d90d1747b92adb28bc367da8f07e33943bc680aa34460`, with Icicle JIT, `-v` and explicit
`-e D:\Sunrise\D2\sogen\root`. This is checkpoint replay, not a fresh-launch
Sunrise boot acceptance test.

- Before: 485.314 seconds to process exit, peak working set
  14,946,246,656 bytes; allocator exhaustion at `ntdll.dll+0x9DB54`.
- After: the complete 122,984,224-byte executable mapped successfully;
  returned view size 122,986,496 bytes at `0xbac47a0000`.
- The syscall returned to `0x104447f66` with RSP advanced by eight.
  Three 4-KiB samples at the beginning, middle and end matched the source file.
  These samples do not constitute a hash of every mapped byte.
- The capture continued after verification. Sunrise boot remains unconfirmed.

Evidence: `destiny-sogen-1789406827015986700-allocator-error-before/held-fault.json`
and `destiny-sogen-1789407816953379500-reclaimed-file-view/file-view-proof.json`
under the PatchScanner benchmark directory. The replay backend and final Release
backend have identical `.text` SHA-256
`bd63b88fb4047e0f5372f4bc9d06036732d1ad0c99151cab0ed06df294d5c186`.
Timing includes verbose logging and different guest scheduling; no throughput
speedup is claimed from these runs.

## Regression checks

- Memory crate: 38 passed, one existing ignored test. Eight new tests cover
  bounded reuse, aliases, partial unmaps, zero pages, virtual roots, snapshots,
  cached code and clean slot reuse.
- Bridge: four reclamation/snapshot/code-remapping tests passed in interpreter
  and JIT modes. The small-pool stress case completes 1,000 unmap/remap cycles.
- Release and required tidy builds passed. The 26 selected C++ regressions
  passed in both interpreter and JIT modes for each build.
- CLI guest smoke: 29/30 passed in 28.647 seconds.
  The existing `APIs` failure still returns guest status 1.
