# Icicle physical-page reclamation

Repeated map/write/unmap operations exhausted Icicle's fixed physical-page pool.
Unmap removed virtual mappings without returning their physical pages to the
allocator. A 16-page pool failed on the fifteenth write despite every earlier
mapping having been removed.

The actual Destiny replay reproduced `MemError::OutOfMemory` with
`physical_pages=800000 capacity=800000`. `NtMapViewOfSection` failed after
105,906,176 of 122,984,224 bytes while mapping the complete on-disk executable.
ViewSize zero requests the remaining section; the request was valid.

Sogen now collects physical-page candidates from removed mappings and reclaims
unreferenced pages at VM boundaries. At the normal capacity, reclamation runs
after 4,096 candidates accumulate, or sooner when the pool is nearly full.
Running JIT code and pending p-code replay defer reclamation. Existing code-cache
invalidation runs before slots can be reused. Capacity is unchanged.

Active aliases, zero pages and executed pages remain protected. Full snapshots
retain their copy-on-write data and matching pending-candidate list. The memory
API accepts additional roots for separately retained virtual address spaces.
Reused slots reset their bytes, permissions and flags.

The local memory dependency retains upstream revision
`3292602fd4857b53ed653dec69398efe4cdb435e`, licenses and formatting configuration.
Its opt-in API leaves ordinary upstream unmap behavior unchanged. No shared Cargo
checkout is modified. `vendor/icicle-mem/UPSTREAM.md` identifies the local changes.

## Validation

- Memory crate: 38 passed, one existing ignored test. Eight new tests cover
  bounded reuse, aliases, partial unmaps, zero pages, virtual roots, snapshots,
  cached code and clean slot reuse.
- Bridge: four reclamation/snapshot/code-remapping tests passed in interpreter
  and JIT modes. The small-pool stress case completes 1,000 unmap/remap cycles.
- Release and required tidy builds passed. The 26 selected C++ regressions
  passed in both interpreter and JIT modes for each build.
- CLI guest smoke: 29/30 passed in 28.647 seconds. The existing `APIs` failure
  still returns guest status 1.

The diagnostic game run exited after 485.314 seconds with a peak working set
of 14,946,246,656 bytes. Evidence is retained in PatchScanner's
`benchmarks/destiny-sogen-1789406827015986700-allocator-error-before/held-fault.json`.

An actual-game replay using the changed backend is still running toward the
full-file mapping checkpoint. Its result is pending; Sunrise boot is unconfirmed.
It restores the same pre-failure snapshot, SHA-256
`c920456cfe4a0f300b4d90d1747b92adb28bc367da8f07e33943bc680aa34460`,
with Icicle JIT, `-v`, and explicit `-e D:\Sunrise\D2\sogen\root`.
This is checkpoint replay, not fresh-launch acceptance. The deployed and final
Release backends have identical `.text` SHA-256
`bd63b88fb4047e0f5372f4bc9d06036732d1ad0c99151cab0ed06df294d5c186`.
