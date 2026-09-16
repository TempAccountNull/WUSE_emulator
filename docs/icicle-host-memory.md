# Icicle host-backed memory

The Vulkan shim's direct-map IOCTL reached `memory_interface::map_host_memory`, which Icicle did not implement. Icicle now maps caller-owned host pages into guest memory. Reads and writes use the same backing, including shared guest aliases; the implementation does not substitute a staging copy.

External pages bypass the guest TLB. Partial mappings retain their backing when a neighboring hole is filled. Explicit host-code cache flushes invalidate every executable alias. A host write to executable bytes still requires that flush. Mapping validates alignment, extent, overlap and physical-page capacity, and rolls back capacity failures before publishing a mapping.

The memory manager assigns a runtime ownership token to each host allocation. Audio storage remains alive while a section or view uses it. Vulkan teardown revokes the allocation's guest mappings and aliases before releasing host storage. Instance cleanup consults the host's recorded device ownership and preserves other instances' mappings. Tokens distinguish an allocation from a later allocation at the same guest address. Partial unmap failures retain ownership and record completed subranges so retries do not unmap them twice. If final GPU cleanup cannot revoke mappings, it logs the affected ranges and retains the Vulkan owner until process exit rather than leaving dangling guest pointers.

## Validation

- Vendored Icicle memory suite: 49 passed, one existing ignored test.
- Memory ownership, host allocation, pagefile views and WARP synchronization: 40 tests passed in both interpreter and JIT modes.
- Real audio RPC/section, GPU final-handle-close and instance-isolation paths: five tests passed in both modes, with no skips. The instance-isolation test failed before the ownership check and passed afterward.
- x64 Vulkan guest fixture: persistent coherent guest write / GPU copy / guest read across 16 submissions, fill and image readback, fences and pipeline-cache checks passed.
- The x86 shim and fixture build successfully. The existing x86/WOW64 loader failure prevents claiming an x86 Vulkan runtime pass.

Focused command: `windows-emulator-test.exe --gtest_filter=HostMemoryLifetimeTest.*:HostMemoryTest.*:HostAllocationTest.*:PagefileSectionViewTest.*:WarpSyncTest.*:AudioHostLifetimeTest.*:GpuHostLifetimeTest.*`. Run with `EMULATOR_ICICLE=1`, the captured emulation root, and each of `SOGEN_ICICLE_JIT=0` and `1`. All 45 cases passed in each mode. Capture identifiers: `regression-1789536326116578100` (final pass), `regression-1789536242554402200` (instance failure before correction), `shim-static-crt-icicle-1789535673815100100` (Vulkan guest test).

## Limits

Host pointers and ownership tokens are never serialized. Icicle rejects snapshots with live external mappings; restore is supported from the pregraphics checkpoint. The earlier audio restore-order problem is not resolved by this change. Other backends must provide atomic individual map operations for map-exception rollback to be safe; WHP/KVM partial-map exception behavior was not validated here.

Each external page still carries Icicle's ordinary inline storage and permission metadata. This is shared backing for correctness, not a reduction in metadata memory use. Large-game graphics allocation overhead must be measured in the actual replay.

These tests establish mapping and cleanup behavior. They do not establish that Destiny reaches its main menu or renders successfully.
