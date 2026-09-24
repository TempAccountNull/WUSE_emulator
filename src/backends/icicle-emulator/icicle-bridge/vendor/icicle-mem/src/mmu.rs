use ahash::AHashSet as HashSet;

use tracing::debug;

use crate::{
    Addr, AllocLayout, IoHandler, IoMemory, IoMemoryAny, MemoryMapping, PhysicalMapping, Snapshot,
    SnapshotData, VirtualMemoryMap,
    perm::{self, MemError, MemResult},
    physical::{self, PageData, PhysicalAddr},
    range_map::RangeMap,
    tlb,
};

pub const DETECT_SELF_MODIFYING_CODE: bool = true;
pub const ENABLE_ZERO_PAGE_OPTIMIZATION: bool = true;
pub const ENABLE_MEMORY_HOOKS: bool = true;

pub trait ReadHook {
    fn read(&mut self, mem: &mut Mmu, addr: u64, size: u8) -> Option<u64>;
}

impl ReadHook for () {
    fn read(&mut self, _: &mut Mmu, _: u64, _: u8) -> Option<u64> {
        None
    }
}

impl<T> ReadHook for T
where
    T: FnMut(&mut Mmu, u64, u8) -> Option<u64>,
{
    fn read(&mut self, mem: &mut Mmu, addr: u64, size: u8) -> Option<u64> {
        self(mem, addr, size)
    }
}

pub trait ReadAfterHook {
    fn read(&mut self, mem: &mut Mmu, addr: u64, value: &[u8]);
}

pub trait WriteHook {
    fn write(&mut self, mem: &mut Mmu, addr: u64, value: &[u8]);

    // Observation runs after each primitive attempt, including early permission/mapping failures.
    // A failed I/O handler may have side effects; Err does not promise unchanged device state.
    fn write_result(&mut self, _mem: &mut Mmu, _addr: u64, _value: &[u8], _result: MemResult<()>) {}

    fn external_write_result(&mut self, mem: &mut Mmu, addr: u64, value: &[u8], result: MemResult<()>) {
        self.write_result(mem, addr, value, result);
    }
}

impl WriteHook for () {
    fn write(&mut self, _: &mut Mmu, _: u64, _: &[u8]) {}
}

impl<T> WriteHook for T
where
    T: FnMut(&mut Mmu, u64, &[u8]),
{
    fn write(&mut self, mem: &mut Mmu, addr: u64, value: &[u8]) {
        self(mem, addr, value);
    }
}

pub struct HookEntry<T: ?Sized> {
    pub start: u64,
    pub end: u64,
    handler: Option<Box<T>>,
}

impl<T: ?Sized> HookEntry<T> {
    // @fixme: Handle case where self.end + page_size overflows.
    fn range(&self, page_size: u64) -> std::ops::RangeInclusive<u64> {
        let alignment_mask = !(page_size - 1);
        let start = self.start & alignment_mask;
        // Align the last covered byte instead of adding a page to the exclusive endpoint.
        let end = (self.end.saturating_sub(1) & alignment_mask) | (page_size - 1);
        start..=end
    }
}

/// A wrapper around a vector with stable ids (removed entries are replaced with a dummy value).
struct HookStore<T: ?Sized> {
    hooks: Vec<HookEntry<T>>,
}

impl<T: ?Sized> HookStore<T> {
    fn new() -> Self {
        Self { hooks: vec![] }
    }

    fn add(&mut self, start: u64, end: u64, handler: Box<T>) -> u32 {
        // Check if there is a dead slot that can be reused.
        let id = match self.hooks.iter().position(|x| x.handler.is_none()) {
            Some(id) => {
                let hook = &mut self.hooks[id];
                hook.start = start;
                hook.end = end;
                hook.handler = Some(handler);
                id as u32
            }
            None => {
                let id = self.hooks.len().try_into().expect("too many hooks");
                self.hooks.push(HookEntry { start, end, handler: Some(handler) });
                id
            }
        };
        id.try_into().expect("too many hooks")
    }

    fn remove(&mut self, id: u32) -> bool {
        let Some(hook) = self.hooks.get_mut(id as usize)
        else {
            return false;
        };
        hook.handler = None;
        hook.start = 0;
        hook.end = 0;
        true
    }

    /// Check if any of the hooks overlap with the page containing `addr`.
    fn contains_address(&self, addr: u64, page_size: u64) -> bool {
        self.hooks.iter().any(|x| x.handler.is_some() && x.range(page_size).contains(&addr))
    }
}

// Compare distances so the access endpoint cannot wrap into low guest addresses.
fn access_overlaps(start: u64, end: u64, addr: u64, size: usize) -> bool {
    start < end && size != 0 && addr < end && (addr >= start || (size as u64) > start - addr)
}

macro_rules! active_hooks {
    ($addr:expr, $size:expr, $list:expr, $action:expr) => {{
        if !$list.hooks.is_empty() {
            let addr = $addr;
            let size = $size;
            let mut hooks = std::mem::take(&mut $list.hooks);
            for hook in &mut hooks {
                if let Some(handler) = hook.handler.as_deref_mut() {
                    if access_overlaps(hook.start, hook.end, addr, size) {
                        ($action)(handler);
                    }
                }
            }
            debug_assert!($list.hooks.is_empty());
            $list.hooks = hooks;
        }
    }};
}

pub struct Mmu {
    // @fixme: actually keep track of memory that has currently been translated.
    pub invalidate_icache: bool,

    // @fixme: this currently triggers to many false positives (e.g. due to vectorized loads which
    // are later masked)
    pub track_uninitialized: bool,

    /// @fixme: handle self-modifying code more carefully.
    pub detect_self_modifying_code: bool,

    pub tlb_hit_count: u64,
    pub tlb_miss_count: u64,
    pub mapping_changed: bool,

    /// A max to apply on operations that might cross page boundaries. This is used to emulate
    /// non-64 bit address spaces.
    address_mask: u64,

    /// The set of virtual (page-aligned) addresses that have been modified since this was last
    /// cleared.
    pub modified: HashSet<u64>,

    /// The translation lookahead buffer for the MMU.
    ///
    /// Note: care needs to be taken to ensure that the relevant entries in this cache are cleared
    /// when the mapping is changed otherwise we may end up with memory safety issues.
    pub tlb: Box<tlb::TranslationCache>,

    /// The current virtual address mapping.
    // @fixme: This should not be public, since changes to this require that the `tlb` is flushed.
    pub mapping: RangeMap<MemoryMapping>,

    /// Unicorn style memory hooks.
    read_hooks: HookStore<dyn ReadHook>,
    read_after_hooks: HookStore<dyn ReadAfterHook>,
    write_hooks: HookStore<dyn WriteHook>,

    /// The underlying physical memory.
    physical: physical::PhysicalMemory,

    /// The parent snapshot for the MMU.
    parent_state: Snapshot,

    /// Registed handlers for I/O memory
    io: Vec<Box<dyn IoMemoryAny>>,

    /// Last IO memory region read -- IO reads are not currently translatable in the JIT, so always
    /// trigger tlb misses. To mitigate some of the performance impact of repeat accesses to the
    /// same address, we keep track of the last IO handler used and check if it matches the address
    /// before doing a search for the region.
    last_io_handler: Option<(u64, u64, IoHandler)>,
}

impl crate::Resettable for Mmu {
    fn reset(&mut self) {
        self.clear();
    }
}

impl Default for Mmu {
    fn default() -> Self {
        Self::with_mask(u64::MAX)
    }
}

impl Mmu {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn with_mask(address_mask: u64) -> Self {
        Self {
            invalidate_icache: false,
            track_uninitialized: false,
            detect_self_modifying_code: DETECT_SELF_MODIFYING_CODE,
            tlb_hit_count: 0,
            tlb_miss_count: 0,
            mapping_changed: false,
            address_mask,
            modified: HashSet::new(),
            tlb: Box::new(tlb::TranslationCache::new()),
            mapping: RangeMap::new(),
            physical: physical::PhysicalMemory::new(physical::MAX_PAGES),
            parent_state: Snapshot::new(SnapshotData::new()),
            io: vec![],

            read_hooks: HookStore::new(),
            read_after_hooks: HookStore::new(),
            write_hooks: HookStore::new(),
            last_io_handler: None,
        }
    }

    pub fn add_write_hook(
        &mut self,
        start: u64,
        end: u64,
        hook: Box<dyn WriteHook>,
    ) -> Option<u32> {
        if start >= end {
            return None;
        }
        self.tlb.clear();
        Some(self.write_hooks.add(start, end, hook))
    }

    pub fn remove_write_hook(&mut self, id: u32) -> bool {
        self.write_hooks.remove(id)
    }

    pub fn get_write_hook(&mut self, id: u32) -> &mut HookEntry<dyn WriteHook> {
        self.tlb.clear();
        &mut self.write_hooks.hooks[id as usize]
    }

    pub fn add_read_hook(&mut self, start: u64, end: u64, hook: Box<dyn ReadHook>) -> Option<u32> {
        if start >= end {
            return None;
        }
        self.tlb.clear();
        Some(self.read_hooks.add(start, end, hook))
    }

    pub fn remove_read_hook(&mut self, id: u32) -> bool {
        self.read_hooks.remove(id)
    }

    pub fn get_read_hook(&mut self, id: u32) -> &mut HookEntry<dyn ReadHook> {
        self.tlb.clear();
        &mut self.read_hooks.hooks[id as usize]
    }

    pub fn add_read_after_hook(
        &mut self,
        start: u64,
        end: u64,
        hook: Box<dyn ReadAfterHook>,
    ) -> Option<u32> {
        if start >= end {
            return None;
        }
        self.tlb.clear();
        Some(self.read_after_hooks.add(start, end, hook))
    }

    pub fn remove_read_after_hook(&mut self, id: u32) -> bool {
        self.read_after_hooks.remove(id)
    }

    pub fn get_read_after_hook(&mut self, id: u32) -> &mut HookEntry<dyn ReadAfterHook> {
        self.tlb.clear();
        &mut self.read_after_hooks.hooks[id as usize]
    }

    pub fn clear(&mut self) {
        self.tlb.clear();
        self.write_hooks.hooks.clear();
        self.read_hooks.hooks.clear();
        self.read_after_hooks.hooks.clear();
        self.mapping = RangeMap::new();
        self.physical.clear();
        self.last_io_handler = None;
    }

    /// Get size (in bytes) of a single page in physical memory.
    #[inline]
    pub fn page_size(&self) -> u64 {
        self.physical.page_size()
    }

    /// Get the offset within a page of an address
    #[inline]
    pub fn page_offset(&self, addr: u64) -> usize {
        physical::PageData::offset(addr)
    }

    /// Align `addr` to a page boundary for the current physical memory configuration.
    #[inline]
    pub fn page_aligned(&self, addr: u64) -> u64 {
        self.physical.page_aligned(addr)
    }

    /// Returns the total number of allocated pages (includes pages referenced by snapshots)
    pub fn total_pages(&self) -> usize {
        self.physical.allocated_pages()
    }

    /// Gets the current physical memory page limit.
    pub fn capacity(&self) -> usize {
        self.physical.capacity()
    }

    /// Sets the maximum number of physical pages the mmu is allowed to allocate.
    ///
    /// Note: If `new_capacity` is smaller than the current number of allocated pages, then the
    /// capacity is set to the number of allocated pages.
    pub fn set_capacity(&mut self, new_capacity: usize) -> bool {
        self.physical.set_capacity(new_capacity)
    }

    /// Read bytes from `addr` checking that the permissions specified by `perm` are set
    pub fn read_bytes(&mut self, mut addr: u64, buf: &mut [u8], perm: u8) -> MemResult<()> {
        if buf.len() > 16 {
            return self.read_bytes_large(addr, buf, perm);
        }

        for byte in buf {
            *byte = self.read::<1>(addr, perm)?[0];
            addr = addr.wrapping_add(1);
        }
        Ok(())
    }

    /// Read bytes from `addr` checking that the permissions specified by `perm` are set
    #[cold]
    pub fn read_bytes_large(&mut self, mut addr: u64, buf: &mut [u8], perm: u8) -> MemResult<()> {
        // Read unaligned bytes at the start
        let aligned_addr = crate::align_up(addr, 16); // @fixme: possible integer overflow
        let (start, buf) = buf.split_at_mut(((aligned_addr - addr) as usize).min(buf.len()));
        for byte in start {
            *byte = self.read::<1>(addr, perm)?[0];
            addr = addr.wrapping_add(1);
        }

        // Read aligned chunks
        let mut chunks = buf.chunks_exact_mut(16);
        for chunk in &mut chunks {
            chunk.copy_from_slice(&self.read::<16>(addr, perm)?);
            addr = addr.wrapping_add(16);
        }

        // Read unaligned bytes at the end
        for byte in chunks.into_remainder() {
            *byte = self.read::<1>(addr, perm)?[0];
            addr = addr.wrapping_add(1);
        }

        Ok(())
    }

    /// Write bytes bytes `addr` checking that the permission specified by `perm` are set and
    /// marking the range written with the `INIT` permission bit.
    pub fn write_bytes(&mut self, mut addr: u64, buf: &[u8], perm: u8) -> MemResult<()> {
        if buf.len() > 16 {
            return self.write_bytes_large(addr, buf, perm);
        }

        for byte in buf {
            self.write(addr, [*byte], perm)?;
            addr = addr.wrapping_add(1);
        }
        Ok(())
    }

    /// Write bytes bytes `addr` checking that the permission specified by `perm` are set and
    /// marking the range written with the `INIT` permission bit.
    #[cold]
    pub fn write_bytes_large(&mut self, mut addr: u64, buf: &[u8], perm: u8) -> MemResult<()> {
        // Write unaligned bytes at the start
        let aligned_addr = crate::align_up(addr, 16); // @fixme: possible integer overflow
        let (start, buf) = buf.split_at(((aligned_addr - addr) as usize).min(buf.len()));
        for byte in start {
            self.write(addr, [*byte], perm)?;
            addr = addr.wrapping_add(1);
        }

        // Write aligned chunks
        let mut chunks = buf.chunks_exact(16);
        for chunk in &mut chunks {
            self.write::<16>(addr, chunk.try_into().unwrap(), perm)?;
            addr = addr.wrapping_add(16);
        }

        // Write unaligned bytes at the end
        for byte in chunks.remainder() {
            self.write(addr, [*byte], perm)?;
            addr = addr.wrapping_add(1);
        }

        Ok(())
    }

    /// Register a handler function that can be mapped to memory locations
    pub fn register_io_handler(&mut self, handler: impl IoMemory + 'static) -> IoHandler {
        let id = self.io.len();
        self.io.push(Box::new(handler));
        IoHandler(id)
    }

    /// Get the memory associated with an I/O handle
    pub fn get_io_memory_mut(&mut self, handler: IoHandler) -> &mut dyn IoMemoryAny {
        &mut *self.io[handler.0]
    }

    #[deprecated(
        note = "The behavior of this function may change in the future. Use `map_memory_len"
    )]
    pub fn map_memory(&mut self, start: u64, end: u64, mapping: impl Into<MemoryMapping>) -> bool {
        self.map_memory_len(start, end - start, mapping)
    }

    /// Attempts to maps a region of memory starting between `start` and `start + len` to `mapping`.
    /// If `start + len` is greater than u64::MAX, memory will wrap around to zero.
    ///
    /// Returns `true` if the memory was succesfully mapped.
    pub fn map_memory_len(
        &mut self,
        start: u64,
        len: u64,
        mapping: impl Into<MemoryMapping>,
    ) -> bool {
        if len == 0 {
            return false; // @todo: should mapping nothing count as being valid?
        }
        let Some(end) = start.checked_add(len - 1)
        else {
            return false;
        };
        let mapping = mapping.into();
        debug!("map_memory: start={:#0x}, end={:#0x}, mapping={:?}", start, end, mapping);

        if let Err(e) = self.mapping.insert(start..=end, mapping) {
            debug!("map_memory: failed: {:0x?}", e);
            return false;
        }
        self.mapping_changed = true;
        self.tlb.remove_range(start, len);
        self.last_io_handler = None;

        true
    }

    /// Maps caller-owned host pages without copying their data.
    ///
    /// # Safety
    /// The host range must remain valid and synchronized until every guest alias is unmapped.
    pub unsafe fn map_host_memory(&mut self, address: u64, pointer: *mut u8, length: u64, permissions: u8) -> bool {
        let page_size = self.page_size();
        if pointer.is_null() || length == 0 || length > isize::MAX as u64
            || address % page_size != 0 || length % page_size != 0
            || (pointer as usize) % page_size as usize != 0 {
            return false;
        }
        let Some(last) = address.checked_add(length - 1) else { return false; };
        if (pointer as usize).checked_add(length as usize).is_none()
            || self.mapping.get_range(address..=last).is_some() {
            return false;
        }
        let mut pages = Vec::new();
        for offset in (0..length).step_by(page_size as usize) {
            let Some(index) = self.physical.alloc() else {
                for (_, index) in pages { self.physical.free(index); }
                return false;
            };
            let page = self.physical.get_mut(index).data_mut();
            page.data = physical::PageBytes::borrowed(std::ptr::NonNull::new_unchecked(pointer.add(offset as usize)));
            page.perm.fill(permissions | perm::MAP | perm::INIT);
            pages.push((address + offset, index));
        }
        for (addr, index) in pages {
            let mapping = PhysicalMapping { addr, index, shared_perm: permissions | perm::MAP | perm::INIT };
            assert!(self.map_memory_len(addr, page_size, MemoryMapping::Physical(mapping)));
        }
        self.clear_tlb();
        true
    }

    /// Map a single guest page backed by an existing shared `Arc<PageData>` for SMP (multi-vCPU
    /// shared RAM). Unlike `map_host_memory` (external, uncachable, ~305x slower), the shared page's
    /// inline bytes are marked `smp_shared`: cachable in the direct-access TLB, writes go through to
    /// the shared bytes, so N per-vCPU MMUs mapping the same `Arc` share one coherent, full-speed
    /// guest page (coherency is the host CPU's). Per-byte permissions come from the shared PageData;
    /// the mapping's `shared_perm` is 0. False on a misaligned address or an overlap.
    pub fn map_smp_shared(&mut self, address: u64, data: std::sync::Arc<PageData>) -> bool {
        // SMP 6.6c'': a (re)map re-establishes this page's perms - bump so stale deferred
        // protects queued against the PREVIOUS mapping skip (see PageData::perm_epoch).
        data.bump_perm_epoch();
        let page_size = self.page_size();
        if address % page_size != 0 {
            return false;
        }
        let Some(last) = address.checked_add(page_size - 1) else { return false; };
        if self.mapping.get_range(address..=last).is_some() {
            return false;
        }
        let Some(index) = self.physical.alloc_shared(data) else { return false; };
        let mapping = PhysicalMapping { addr: address, index, shared_perm: 0 };
        assert!(self.map_memory_len(address, page_size, MemoryMapping::Physical(mapping)));
        self.clear_tlb();
        true
    }

    fn can_map_smp_page_range(&self, address: u64, count: usize) -> bool {
        let page_size = self.page_size();
        if address % page_size != 0 {
            return false;
        }
        if count == 0 {
            return true;
        }
        let Some(length) = u64::try_from(count).ok().and_then(|n| n.checked_mul(page_size)) else {
            return false;
        };
        let Some(last) = address.checked_add(length - 1) else { return false; };
        self.mapping.get_range(address..=last).is_none()
    }

    /// Map a contiguous captured SMP range with one virtual-map insertion and one TLB clear.
    /// An overlapping range is rejected before any page is mapped. On allocation failure, retain
    /// the successfully allocated prefix, as repeated `map_smp_shared` did.
    pub fn map_smp_shared_pages(&mut self, address: u64, pages: &[std::sync::Arc<PageData>]) -> bool {
        if !self.can_map_smp_page_range(address, pages.len()) {
            return false;
        }

        let page_size = self.page_size();
        let mut entries = Vec::with_capacity(pages.len());
        let mut success = true;
        for (offset, data) in pages.iter().enumerate() {
            data.bump_perm_epoch();
            // The checked full-range preflight above proves these addresses cannot overflow.
            let start = address + offset as u64 * page_size;
            let last = start + page_size - 1;
            let Some(index) = self.physical.alloc_shared(std::sync::Arc::clone(data)) else {
                success = false;
                break;
            };
            let mapping = PhysicalMapping { addr: start, index, shared_perm: 0 };
            entries.push((start, last, MemoryMapping::Physical(mapping)));
        }

        if !entries.is_empty() {
            // Every entry is one page, addresses are increasing, and PhysicalMapping.addr makes
            // adjacent values unequal. Existing overlaps were checked in the same order as before.
            assert!(self.mapping.insert_disjoint_batch(&entries));
            self.mapping_changed = true;
            self.clear_tlb();
        }
        success
    }

    /// Allocate and map a fresh contiguous SMP range using the same single-splice path.
    pub fn map_smp_shared_fresh_pages(&mut self, address: u64, count: usize, permissions: u8) -> bool {
        if count == 0 || !self.can_map_smp_page_range(address, count) {
            return false;
        }
        let mut pages = Vec::with_capacity(count);
        for _ in 0..count {
            let mut data = PageData::default();
            data.perm.fill(permissions | perm::MAP | perm::INIT);
            pages.push(std::sync::Arc::new(data));
        }
        self.map_smp_shared_pages(address, &pages)
    }

    /// Create and map a fresh shared page (SMP) with the given internal permission bits. Other vCPUs
    /// map the same page via `share_page` + `map_smp_shared`.
    pub fn map_smp_shared_fresh(&mut self, address: u64, permissions: u8) -> bool {
        let mut page_data = PageData::default();
        page_data.perm.fill(permissions | perm::MAP | perm::INIT);
        self.map_smp_shared(address, std::sync::Arc::new(page_data))
    }

    /// Clone the shared `Arc<PageData>` backing the page at `address`, so another MMU can map the
    /// same physical page via `map_smp_shared` (SMP). None if the address is not mapped.
    pub fn share_page(&self, address: u64) -> Option<std::sync::Arc<PageData>> {
        let index = self.get_physical_index(address)?;
        Some(self.physical.share_page_data(index))
    }

    pub fn has_host_mappings(&self) -> bool {
        self.mapping.iter().any(|(_, _, mapping)| match mapping {
            MemoryMapping::Physical(page) => self.physical.get(page.index).data().data.is_external(),
            _ => false,
        })
    }

    pub fn host_mapping_aliases(&self, pointer: usize, length: usize) -> Vec<(u64, u64)> {
        let Some(end) = pointer.checked_add(length) else { return Vec::new(); };
        self.mapping.iter().filter_map(|(start, last, mapping)| {
            let MemoryMapping::Physical(page) = mapping else { return None; };
            let host = self.physical.get(page.index).data().data.host_address()?;
            let host_start = host + PageData::offset(start);
            let host_end = host_start + (last - start + 1) as usize;
            (host_start < end && pointer < host_end).then_some((start, last - start + 1))
        }).collect()
    }

    pub fn map_shared(&mut self, dst: u64, src: u64, len: u64, permissions: u8) -> MemResult<()> {
        let page_size = self.page_size();
        if len == 0 || (dst | src | len) & (page_size - 1) != 0 {
            return Err(MemError::Unaligned);
        }
        let end = dst.checked_add(len - 1).ok_or(MemError::AddressOverflow)?;
        src.checked_add(len - 1).ok_or(MemError::AddressOverflow)?;
        if self.mapping.overlapping_iter(dst..=end).any(|(_, _, entry)| entry.is_some()) {
            return Err(MemError::Unmapped);
        }
        for offset in (0..len).step_by(page_size as usize) {
            match self.mapping.get_with_range(src + offset) {
                Some((start, end, MemoryMapping::Physical(_) | MemoryMapping::Unallocated(_)))
                    if start <= src + offset && end >= src + offset + page_size - 1 => {}
                _ => return Err(MemError::Unmapped),
            }
        }
        let mut pages = Vec::new();
        for offset in (0..len).step_by(page_size as usize) {
            let address = src + offset;
            let original_perm = self.get_perm(address) | perm::MAP | perm::INIT;
            if let Some(index) = self.get_physical_index(address) {
                if index.is_zero_page() {
                    self.physical.get_mut(index).copy_on_write = true;
                    self.tlb.remove_write(address);
                }
            }
            let value = self.read::<1>(address, perm::NONE)?;
            self.write(address, value, perm::NONE)?;
            let index = self.get_physical_index(address).ok_or(MemError::Unmapped)?;
            self.mapping.overlapping_mut::<_, MemError>(address..=address + page_size - 1,
                |_, _, entry| {
                    if let Some(MemoryMapping::Physical(mapping)) = entry {
                        mapping.shared_perm = original_perm;
                    }
                    Ok(())
                })?;
            pages.push(index);
        }
        self.tlb.clear();
        for (offset, index) in pages.into_iter().enumerate() {
            let addr = dst + offset as u64 * page_size;
            let mapping = PhysicalMapping {
                addr, index, shared_perm: permissions | perm::MAP | perm::INIT,
            };
            assert!(self.map_memory_len(addr, page_size, MemoryMapping::Physical(mapping)));
        }
        Ok(())
    }

    pub fn map_physical(&mut self, addr: u64, index: physical::Index) -> bool {
        self.map_memory_len(
            addr,
            self.page_size(),
            MemoryMapping::Physical(PhysicalMapping { index, addr, shared_perm: 0 }),
        )
    }

    /// Unmaps the region of memory between `start` and `start+len`
    #[deprecated(
        note = "The behavior of this function may change in the future. Use `unmap_memory_len`"
    )]
    pub fn unmap_memory(&mut self, start: u64, end: u64) -> bool {
        self.unmap_memory_len(start, start - end)
    }

    /// Unmaps the region of memory between `start` and `start+len`
    pub fn unmap_memory_len(&mut self, start: u64, len: u64) -> bool {
        if len == 0 {
            return false; // @todo: should unmapping nothing count as being valid?
        }
        let Some(end) = start.checked_add(len - 1)
        else {
            return false;
        };

        debug!("unmap_memory: start={:#0x}, end={:#0x}", start, end);
        self.mapping_changed = true;

        let physical = &mut self.physical;
        let tlb = &mut self.tlb;
        let mut partially_unmapped = false;
        let _ = self.mapping.overlapping_mut::<_, ()>(start..=end, |start, len, entry| {
            tracing::trace!("unmap: ({:#0x}, {:#0x}): {:0x?}", start, len, entry);
            match entry.take() {
                Some(MemoryMapping::Physical(inner)) => {
                    tlb.remove_range(start, len);
                    if inner.shared_perm != 0 || len == physical.page_size() {
                        return Ok(());
                    }

                    // Clear permissions for the unmapped region.
                    //
                    // @fixme: this page could potentially be mapped in multiple locations,
                    // resulting in mapping issues.
                    let page = physical.get_mut(inner.index);
                    assert!(!page.executed, "Unmapped cached code page. Currently unsupported");

                    let offset = PageData::offset(start);
                    if page.smp_shared {
                        // SMP: in-place perm clear on the shared page (a clone would privatize this
                        // VM's view of a page the guest is unmapping — peers must see the same state).
                        // Safety: smp_shared pages are never cloned; see Page::data_mut_shared.
                        // SMP: do NOT touch shared byte-perms on unmap (never clone either -
                        // that privatizes this VM's view). Unmap is PER-VM intent: the mapping-tree
                        // removal below already blocks THIS VM; clearing SHARED perms would break
                        // peers still mapping the page (probe's loader write failed exactly so:
                        // peer unmap -> shared perms NONE -> master module write ice-throw).
                    } else {
                        page.data_mut().perm[offset..offset + len as usize].fill(perm::NONE);
                    }
                }
                Some(_) => {}

                // Attempted to unmap region that wasn't mapped
                None => partially_unmapped = true,
            }

            Ok(())
        });

        !partially_unmapped
    }

    /// Reclaims removed-page candidates that have no remaining virtual mapping or cached code.
    /// Any virtual address spaces retained outside this MMU must be supplied as additional roots.
    pub fn reclaim_unmapped_physical(
        &mut self,
        candidates: &[physical::Index],
        additional_roots: &[&VirtualMemoryMap],
    ) -> usize {
        let mut unused: HashSet<_> =
            candidates.iter().copied().filter(|index| !index.is_zero_page()).collect();
        for mapping in std::iter::once(&self.mapping).chain(additional_roots.iter().copied()) {
            for (_, _, entry) in mapping.iter() {
                if let MemoryMapping::Physical(page) = entry {
                    unused.remove(&page.index);
                }
            }
        }

        let mut released = 0;
        for &index in candidates {
            if unused.remove(&index) && !self.physical.get(index).executed {
                self.physical.free(index);
                released += 1;
            }
        }
        if released != 0 {
            self.tlb.clear();
        }
        released
    }

    /// Allocates `count` physical pages, returning an error if we are out of memory.
    pub fn alloc_physical(&mut self, count: usize) -> MemResult<Vec<physical::Index>> {
        debug!("alloc_physical: count={count}");
        (0..count).map(|_| self.physical.alloc().ok_or(MemError::OutOfMemory)).collect()
    }

    /// Finds a free region of memory satisfying `layout` then map it to `mapping`
    pub fn alloc_memory(
        &mut self,
        layout: AllocLayout,
        mapping: impl Into<MemoryMapping>,
    ) -> MemResult<u64> {
        let mapping = mapping.into();
        debug!("alloc_memory: layout={layout:0x?}, mapping={mapping:?}");

        let start = self.find_free_memory(layout)?;
        self.map_memory_len(start, layout.size, mapping);
        Ok(start)
    }

    /// Finds a free region of memory satisfying `layout`
    pub fn find_free_memory(&self, layout: AllocLayout) -> MemResult<u64> {
        // Compute the length that we will end up with if we add the padding necessary to meet
        // alignment constraints
        let align = layout.align.checked_next_power_of_two().unwrap();
        let aligned_length = crate::align_up(layout.size, align);

        // Either use the preferred address specified in the layout or start at the lowest address
        // available.
        let mut start_addr = crate::align_up(layout.addr.unwrap_or(0), align);

        while let Some((_, end)) = self.mapping.get_range(
            start_addr..=start_addr.checked_add(aligned_length - 1).ok_or(MemError::OutOfMemory)?,
        ) {
            start_addr = crate::align_up(end + 1, align);
        }

        Ok(start_addr)
    }

    /// Updates the mapping value associated with a region of memory
    pub fn update_perm(&mut self, addr: u64, count: u64, perm: u8) -> MemResult<()> {
        let end = addr.checked_add(count - 1).ok_or(MemError::AddressOverflow)?;
        let perm =
            perm | perm::MAP | if self.track_uninitialized { perm::NONE } else { perm::INIT };
        debug!("update_perm: addr={addr:#0x}, count={count:#0x}, perm={}", perm::display(perm));

        self.mapping_changed = true;

        let physical = &mut self.physical;
        let tlb = &mut self.tlb;
        self.mapping.overlapping_mut(addr..=end, |start, len, entry| {
            match entry.as_mut().ok_or(MemError::Unmapped)? {
                MemoryMapping::Physical(entry) => 'physical: {
                    tlb.remove_range(start, len);

                    if entry.shared_perm != 0 {
                        entry.shared_perm = perm;
                        return Ok(());
                    }
                    let offset = PageData::offset(start);
                    let len = len as usize;

                    if offset == 0 && len == physical::PAGE_SIZE && entry.index.is_zero_page() {
                        if let Some(zero_page) = physical.get_zero_page(perm) {
                            debug!("updating zero page: {:?} -> {zero_page:?}", entry.index);
                            entry.index = zero_page;
                            break 'physical;
                        }
                    }

                    let page = physical.get_mut(entry.index);
                    if page.executed {
                        tracing::error!("Changed perms of code page. JIT cache may now be invalid");
                    }
                    if page.smp_shared {
                        if perm & perm::EXEC != 0 {
                            page.data().smp_code_seen.store(1, std::sync::atomic::Ordering::Release);
                        }
                        // SMP: never make_mut a shared page — the clone privatizes this VM's copy
                        // (protect_does_not_privatize_shared_page: the protecting VM loses all
                        // subsequent peer writes; the N>1 probe's WritePerm fault on ntdll .data
                        // traces to this). Protection is a property of the shared page: apply the
                        // perm bytes in place so every sharing VM sees the same protection.
                        // Safety: smp_shared pages are never cloned; see Page::data_mut_shared.
                        let data = unsafe { page.data_mut_shared() };
                        data.perm[offset..offset + len].fill(perm);
                        data.bump_perm_epoch();
                    } else {
                        page.data_mut().perm[offset..offset + len].fill(perm);
                    }
                }
                MemoryMapping::Unallocated(entry) => entry.perm = perm,
                MemoryMapping::Io(_) => {
                    unimplemented!("attempted to update permission of I/O region")
                }
            }

            Ok(())
        })
    }

    /// Fill a region of memory with `value`
    pub fn fill_mem(&mut self, addr: u64, count: u64, value: u8) -> MemResult<()> {
        if count == 0 {
            return Ok(());
        }
        let end = addr.checked_add(count - 1).ok_or(MemError::AddressOverflow)?;
        debug!("fill_mem: addr={:#0x}, count={:#0x}, value={:#0x}", addr, count, value);
        if self.mapping.overlapping_iter(addr..=end).any(|(_, _, entry)|
            matches!(entry, Some(MemoryMapping::Physical(page)) if page.shared_perm != 0)) {
            for offset in 0..count {
                self.write::<1>(addr + offset, [value], perm::NONE)?;
            }
            return Ok(());
        }

        let physical = &mut self.physical;
        let tlb = &mut self.tlb;
        self.mapping.overlapping_mut(addr..=end, |start, len, entry| {
            match entry.as_mut().ok_or(MemError::Unmapped)? {
                MemoryMapping::Physical(entry) => {
                    tlb.remove_range(start, len);
                    let page = physical.get_mut(entry.index);
                    if page.executed && self.detect_self_modifying_code {
                        check_self_modifying_memset(page.data(), start, len, value)?;
                    }

                    let offset = PageData::offset(start);

                    // Check whether we a simply overwritting a zero page with zeros.
                    let write_zero_to_zero_page = value == 0
                        && offset == 0
                        && len as usize == physical::PAGE_SIZE
                        && entry.index.is_zero_page();

                    if !write_zero_to_zero_page {
                        let page = page.data_mut();
                        page.data[offset..offset + len as usize].fill(value);
                        page.add_perm(offset, len as usize, perm::INIT);
                    }
                }
                MemoryMapping::Unallocated(entry) => {
                    entry.value = value;
                    entry.perm |= perm::INIT;
                }
                MemoryMapping::Io(_) => {
                    unimplemented!("attempted to memset an I/O region")
                }
            }
            Ok(())
        })
    }

    #[deprecated(
        note = "The behavior of this function may change in the future. Use `move_region_len`"
    )]
    pub fn move_region(&mut self, start: u64, end: u64, dst: u64) -> MemResult<()> {
        self.move_region_len(start, end - start, dst)
    }

    pub fn move_region_len(&mut self, start: u64, len: u64, dst: u64) -> MemResult<()> {
        let offset = dst as i64 - start as i64;
        let mut end = start.checked_add(len - 1).ok_or(MemError::AddressOverflow)?;

        while start < end {
            let (prev, (overlap_start, overlap_end)) =
                self.mapping.remove_last(start..=end).ok_or(MemError::Unmapped)?;

            if overlap_end < end {
                return Err(MemError::Unmapped);
            }

            self.tlb.remove_range(overlap_start, (overlap_end - overlap_start) + 1);
            self.last_io_handler = None;

            let shifted_start = (overlap_start as i64 + offset) as u64;
            let shifted_end = (overlap_end as i64 + offset) as u64;
            self.mapping.insert((shifted_start, shifted_end), prev).unwrap();

            end = overlap_start
        }
        Ok(())
    }

    /// Clear the translation lookahead buffer.
    pub fn clear_tlb(&mut self) {
        self.tlb.clear();
        self.last_io_handler = None;
    }

    /// Obtain a raw pointer to the translation lookahead buffer.
    ///
    /// Safety: Avoid any operation except reading/writing to initialized memory locations while
    /// this pointer is active.
    pub fn tlb_ptr(&mut self) -> *const tlb::TranslationCache {
        self.tlb.as_ref() as *const _
    }

    /// Invalidate an entry in the TLB.
    pub fn invalidate_page(&mut self, addr: u64) {
        self.tlb.remove(addr);
    }

    /// Create a full snapshot of memory that can later be restored
    pub fn snapshot(&mut self) -> Snapshot {
        assert!(!self.has_host_mappings(), "Cannot snapshot caller-owned host memory");
        // TLB is invalidated whenever we clone the physical memory state.
        self.tlb.clear();

        let snapshot = SnapshotData {
            mapping: self.mapping.clone(),
            physical: self.physical.snapshot(),
            parent: Some(self.parent_state.clone()),
            io: self.io.iter_mut().map(|x| x.snapshot()).collect(),
        };

        // Reconfigure the current modification state to be tracked based on the new snapshot
        self.parent_state = std::sync::Arc::new(snapshot);
        self.parent_state.clone()
    }

    /// Restore the full memory state from `snapshot`
    pub fn restore(&mut self, snapshot: Snapshot) {
        self.tlb.clear();
        self.last_io_handler = None;

        self.modified.clear();
        self.mapping_changed = true;

        self.physical.restore(&snapshot.physical);
        self.io.iter_mut().zip(&snapshot.io).for_each(|(io, snapshot)| io.restore(snapshot));

        // Configure our state to match the snapshot
        self.mapping.clone_from(&snapshot.mapping);
        self.parent_state = snapshot;
    }

    /// Create a snapshot of just the virtual address space
    pub fn snapshot_virtual_mapping(&mut self) -> VirtualMemoryMap {
        assert!(!self.has_host_mappings(), "Cannot snapshot caller-owned host memory");
        // Clear the TLB to ensure that no writes will be missed.
        self.tlb.clear();
        self.last_io_handler = None;

        // Mark all physical pages in the mapping as copy-on-write.
        for (_, _, entry) in self.mapping.iter() {
            if let MemoryMapping::Physical(mapping) = entry {
                self.physical.get_mut(mapping.index).copy_on_write = true;
            }
        }

        self.mapping.clone()
    }

    /// Take the underlying virtual address space.
    pub fn take_virtual_mapping(&mut self) -> VirtualMemoryMap {
        self.tlb.clear();
        self.last_io_handler = None;
        self.mapping_changed = true;
        std::mem::take(&mut self.mapping)
    }

    /// Restore just the virtual address space
    pub fn restore_virtual_mapping(&mut self, mapping: VirtualMemoryMap) {
        self.mapping = mapping;
        self.tlb.clear();
        self.last_io_handler = None;

        self.modified.clear();
        self.mapping_changed = true;
    }

    /// Reset the the virtual address space
    pub fn reset_virtual(&mut self) {
        self.mapping.clear();
        self.tlb.clear();
        self.last_io_handler = None;

        self.modified.clear();
        self.mapping_changed = true;
    }

    /// Clear the page modification log
    pub fn clear_page_modification_log(&mut self) {
        self.tlb.clear_write();
        self.last_io_handler = None;
        self.modified.clear();
    }

    /// Get the permission bits associated with the byte at `addr`
    pub fn get_perm(&self, addr: u64) -> u8 {
        let entry = match self.mapping.get(addr) {
            Some(entry) => entry,
            None => return perm::NONE,
        };
        match entry {
            MemoryMapping::Physical(entry) => {
                if entry.shared_perm != 0 {
                    return entry.shared_perm;
                }
                let page = self.physical.get(entry.index).data();
                let (offset, _) = PageData::offset_and_len(addr, addr + 1);
                page.perm[offset]
            }
            MemoryMapping::Unallocated(metadata) => metadata.perm,
            MemoryMapping::Io(_) => {
                // @fixme?
                perm::NONE
            }
        }
    }

    /// Check that the region of memory between addr..addr+len is initialized and executable, and
    /// ensure that if it is ever written to in the future it will be detected.
    pub fn ensure_executable(&mut self, start: u64, len: u64) -> bool {
        let Some(end) = start.checked_add(len - 1)
        else {
            return false;
        };

        let tlb = &mut self.tlb;
        let physical = &mut self.physical;
        self.mapping
            .overlapping_mut::<_, MemError>(start..=end, |start, len, entry| match entry {
                Some(MemoryMapping::Physical(mapping)) => {
                    let page = physical.get_mut(mapping.index);

                    // Check whether the code is actually executable.
                    let offset = PageData::offset(start);
                    let len = len as usize;
                    let perm =
                        unsafe { page.write_ptr().ptr.as_mut().get_perm_unchecked(offset, len) };
                    perm::check(if mapping.shared_perm != 0 { mapping.shared_perm } else { perm },
                        perm::INIT | perm::EXEC)?;

                    if page.smp_shared {
                        page.data().smp_code_seen.store(1, std::sync::atomic::Ordering::Release);
                    }

                    // Mark the page as executed
                    page.executed = true;

                    // Prevent writes to the region we are executing (we don't currently support
                    // self modifying code).
                    if self.detect_self_modifying_code {
                        unsafe {
                            page.write_ptr().ptr.as_mut().add_perm_unchecked(
                                offset,
                                len,
                                perm::IN_CODE_CACHE,
                            );
                        };
                    }

                    if mapping.shared_perm != 0 { tlb.clear_write(); }
                    else { tlb.remove_write(mapping.addr); }
                    Ok(())
                }
                _ => Err(MemError::ExecViolation),
            })
            .is_ok()
    }

    /// Clears the executable bit from uninitialized memory.
    ///
    /// @fixme: this was used a workaround for `track_uninitialized` returning to many false
    /// positives in some cases.
    pub fn clear_uninitialized_exec_bytes(&mut self) {
        let physical = &mut self.physical;
        for (start, end, entry) in self.mapping.iter_mut() {
            match entry {
                MemoryMapping::Physical(entry) => {
                    if entry.shared_perm != 0 { continue; }
                    let (offset, len) = PageData::offset_and_len(start, end + 1);
                    let page = physical.get_mut(entry.index);
                    if page.smp_shared {
                        // SMP: in-place (a clone would privatize this VM's view of a shared page).
                        // Safety: smp_shared pages are never cloned; see Page::data_mut_shared.
                        unsafe { page.data_mut_shared() }.perm[offset..offset + len].iter_mut().for_each(|p| {
                            if *p & perm::INIT == 0 {
                                *p &= !perm::EXEC;
                            }
                        });
                    } else {
                        page.data_mut().perm[offset..offset + len].iter_mut().for_each(|p| {
                            if *p & perm::INIT == 0 {
                                *p &= !perm::EXEC;
                            }
                        });
                    }
                }
                MemoryMapping::Unallocated(x) => x.perm &= !perm::EXEC,
                MemoryMapping::Io(_) => {}
            }
        }
    }

    /// Initialize a new physical page and map it such that it contains `addr`.
    ///
    /// Returns the index of the new page in physical memory (or `None` if we are out of memory)
    fn init_physical(&mut self, addr: u64, is_write: bool) -> Option<physical::Index> {
        let page_start = self.page_aligned(addr);
        let page_size = self.page_size();
        let page_end = page_start + (page_size - 1);

        let range = page_start..=page_end;
        // If we are only reading from this page and the entire region is entirely zero, then map it
        // to a zero page.
        if ENABLE_ZERO_PAGE_OPTIMIZATION && !is_write {
            if let Some(zero_page) = self.get_zero_page(page_start, page_size) {
                tracing::trace!("init_physical: addr={page_start:#0x}, index={zero_page:?}");

                let _ = self.mapping.overlapping_mut::<_, ()>(range, |_, _, entry| {
                    *entry = Some(MemoryMapping::Physical(PhysicalMapping {
                        index: zero_page,
                        addr: page_start,
                        shared_perm: 0,
                    }));
                    Ok(())
                });
                return Some(zero_page);
            }
        }

        let index = self.physical.alloc()?;
        self.tlb.remove(page_start);

        tracing::trace!("init_physical: addr={:#0x}, index={:?}", page_start, index);
        let new_mapping = PhysicalMapping { index, addr: page_start, shared_perm: 0 };

        let init_perm = if self.track_uninitialized { perm::NONE } else { perm::INIT };

        let physical = &mut self.physical;
        let _ = self.mapping.overlapping_mut::<_, ()>(range, |start, len, entry| {
            let len = len as usize;

            // Determine how this region of the page should be initalized.
            let (value, perm) = match entry {
                Some(MemoryMapping::Unallocated(x)) => {
                    tracing::trace!("Replacing unallocated region (start={start:#x}, len={len:#x}) with physical mapping.");
                    let init = (x.value, x.perm | perm::MAP | init_perm);
                    *entry = Some(MemoryMapping::Physical(new_mapping));
                    init
                }
                // Preserve the backing identity of surviving aliases when filling a hole in their page.
                Some(MemoryMapping::Physical(existing))
                    if existing.shared_perm != 0 || physical.get(existing.index).data().data.is_external() =>
                {
                    (crate::UNINIT_VALUE, perm::NONE)
                }
                Some(MemoryMapping::Physical(existing)) => {
                    // Rare case where there was an existing page map at this location. This should
                    // only occur when a page is partially mapped. Copy any memory that could be
                    // lost when we replace this mapping.
                    //
                    // @fixme: handle this better.

                    tracing::trace!(
                        "copy {len:#0x} bytes at {start:#0x} from: {:?}",
                        existing.index
                    );

                    let offset = (start - page_start) as usize;

                    let (old_page, new_page) = physical.get_pair_mut(existing.index, index);
                    let (old, new) = (old_page.data(), new_page.data_mut());
                    new.data[offset..offset + len].copy_from_slice(&old.data[offset..offset + len]);
                    new.perm[offset..offset + len].copy_from_slice(&old.perm[offset..offset + len]);

                    *entry = Some(MemoryMapping::Physical(new_mapping));
                    return Ok(());
                }
                Some(MemoryMapping::Io(_)) => (crate::UNINIT_VALUE, perm::NONE),
                None => (crate::UNINIT_VALUE, perm::NONE),
            };

            let page = physical.get_mut(index).data_mut();
            let offset = PageData::offset(start);
            page.data[offset..offset + len].fill(value);
            page.perm[offset..offset + len].fill(perm);

            Ok(())
        });

        Some(index)
    }

    /// Checks whether the memory is zero page compatible, returning the index of the zero page.
    fn get_zero_page(&self, start: u64, len: u64) -> Option<physical::Index> {
        let end = start.checked_add(len - 1)?;
        let mut perm = None;
        for (_, _, entry) in self.mapping.overlapping_iter(start..=end) {
            match entry {
                Some(MemoryMapping::Unallocated(x)) if x.is_zero() => {
                    if perm.map_or(false, |perm| x.perm != perm) {
                        return None;
                    }
                    perm = Some(x.perm);
                }
                _ => return None,
            }
        }
        perm.and_then(|perm| self.physical.get_zero_page(perm))
    }

    /// Checks whether the memory range entirely consists of mapped regular memory.
    pub fn is_regular_region(&self, start: u64, len: u64) -> bool {
        let Some(end) = start.checked_add(len - 1)
        else {
            return false;
        };
        for (_, _, entry) in self.mapping.overlapping_iter((start, end)) {
            match entry {
                Some(MemoryMapping::Physical(_) | MemoryMapping::Unallocated(_)) => {}
                _ => return false,
            }
        }
        true
    }

    /// Gets the physical address assocated with a virtual address, returning `None` if `addr` is
    /// unmapped or unallocated
    pub fn get_physical_addr(&self, addr: u64) -> Option<PhysicalAddr> {
        self.resolve_vaddr(addr).map(|entry| entry.phys)
    }

    pub fn resolve_vaddr(&self, vaddr: u64) -> Option<Addr> {
        match self.mapping.get(vaddr)? {
            MemoryMapping::Physical(entry) => {
                Some(Addr { virt: vaddr, phys: self.physical.address_of(vaddr, entry.index) })
            }
            _ => None,
        }
    }

    /// Get the index of physical page mapped at `addr`.
    pub fn get_physical_index(&self, addr: u64) -> Option<physical::Index> {
        match self.mapping.get(addr)? {
            MemoryMapping::Physical(entry) => Some(entry.index),
            _ => None,
        }
    }

    pub fn get_physical(&self, index: physical::Index) -> &physical::Page {
        self.physical.get(index)
    }

    pub fn get_physical_mut(&mut self, index: physical::Index) -> &mut physical::Page {
        // @fixme: this may invalidate the TLB
        self.physical.get_mut(index)
    }

    fn read_physical<const N: usize>(
        &mut self,
        index: physical::Index,
        addr: u64,
        perm: u8,
    ) -> MemResult<[u8; N]> {
        let page_size = self.page_size();
        let page = self.physical.get_mut(index);
        let shared_perm = match self.mapping.get_with_range(addr) {
            Some((_, last, MemoryMapping::Physical(mapping))) if last - addr >= (N - 1) as u64 => mapping.shared_perm,
            _ => return Err(MemError::Unmapped),
        };
        if shared_perm != 0 { perm::check(shared_perm, perm)?; }
        let result = page.data().read(addr, if shared_perm != 0 { perm::NONE } else { perm })?;

        // If there is no memory hook set on the current page, cache the translated address in the
        // TLB.
        let uncachable = shared_perm != 0 || page.data().data.is_external() || self.read_hooks.contains_address(addr, page_size)
            || self.read_after_hooks.contains_address(addr, page_size);
        if !uncachable {
            self.tlb.insert_read(addr, unsafe { page.read_ptr() });
        }
        Ok(result)
    }

    fn write_physical<const N: usize>(
        &mut self,
        index: physical::Index,
        addr: u64,
        value: [u8; N],
        perm: u8,
    ) -> MemResult<()> {
        // SMP 6.6a experiment (SOGEN_SMP_EPOCH=2): bump the epoch for PRIVATE (executed) pages too,
        // so the same raise/recovery can be A/B tested at N=1 on a private page.
        if std::env::var("SOGEN_SMP_EPOCH").map(|v| v == "2").unwrap_or(false) {
            let page = self.physical.get(index);
            if page.executed {
                page.data().bump_code_epoch();
            }
        }
        let page_start = self.page_aligned(addr);
        let page_size = self.page_size();

        let shared_perm = match self.mapping.get_with_range(addr) {
            Some((_, last, MemoryMapping::Physical(mapping))) if last - addr >= (N - 1) as u64 => mapping.shared_perm,
            _ => return Err(MemError::Unmapped),
        };
        if shared_perm != 0 { perm::check(shared_perm, perm)?; }
        let mut page = self.physical.get_mut(index);
        if page.smp_shared {
            // SMP shared page: write straight through to the shared Arc<PageData> (no copy-on-write)
            // so every vCPU sharing this page sees it. The shared byte pointer is stable (never
            // make_mut'd), so it stays cachable in the direct-access TLB — full JIT speed, unlike the
            // shared_perm/external path. Cross-vCPU coherency is the host CPU's (MESI).
            if page.executed && self.detect_self_modifying_code {
                check_self_modifying_write(page.data(), addr, &value)?;
            }
            self.tlb.remove_read(page_start);
            if !page.modified {
                self.modified.insert(page_start);
                page.modified = true;
            }
            // Safety: shared bytes; concurrent access across vCPUs is coordinated by host coherency.
            let shared = unsafe { page.data_mut_shared() };
            // SMP 6.6a: if the written bytes were translated (IN_CODE_CACHE in the SHARED perms — set
            // by ANY VM's lifter), bump the page's cross-VM code epoch. Peers validate the epoch per
            // block execution and flush their stale translation. This is the path GUEST stores take
            // (including cached TLB write pointers on re-entry), which host-side fan-out cannot see.
            let was_cached = shared.perm[PageData::offset(addr)..PageData::offset(addr) + N]
                .iter()
                .any(|p| p & perm::IN_CODE_CACHE != 0);
            shared.write(addr, value, perm)?;
            if was_cached
                || shared.smp_code_seen.load(std::sync::atomic::Ordering::Acquire) != 0
            {
                shared.bump_code_epoch();
            }
            if !self.write_hooks.contains_address(addr, page_size) {
                self.tlb.insert_write(page_start, unsafe { page.shared_write_ptr() });
            }
            return Ok(());
        }
        if page.executed && self.detect_self_modifying_code {
            check_self_modifying_write(page.data(), addr, &value)?;
        }

        if page.copy_on_write {
            // Make a copy and update the mapping to point to the new copy.
            let copy_index = self.physical.clone_page(index).ok_or(MemError::OutOfMemory)?;
            let copy_mapping = PhysicalMapping { index: copy_index, addr: page_start, shared_perm: 0 };
            tracing::trace!("{:?} ({:#0x}) copy-on-write -> {copy_index:?}", index, page_start);

            if shared_perm != 0 {
                for (_, _, entry) in self.mapping.iter_mut() {
                    if let MemoryMapping::Physical(mapping) = entry {
                        if mapping.index == index && mapping.shared_perm != 0 {
                            mapping.index = copy_index;
                        }
                    }
                }
                self.tlb.clear();
            } else {
                let page_end = page_start + (page_size - 1);
                self.mapping.overlapping_mut(page_start..=page_end, |_start, _end, entry| {
                    if let Some(mapping @ MemoryMapping::Physical(_)) = entry {
                        *mapping = MemoryMapping::Physical(copy_mapping);
                    }
                    Ok(())
                })?;
            }

            page = self.physical.get_mut(copy_index);
        }

        // `data_mut` may cause a new copy of page to be created, so invalidate the read entry for
        // the TLB cache.
        self.tlb.remove_read(page_start);

        // @todo: check the overhead of this hash operation.

        if !page.modified {
            self.modified.insert(page_start);
        }
        page.modified = true;
        page.data_mut().write(addr, value, if shared_perm != 0 { perm::NONE } else { perm })?;

        let uncachable = shared_perm != 0 || page.data().data.is_external() || self.write_hooks.contains_address(addr, page_size);
        if shared_perm != 0 { self.tlb.clear(); }
        if !uncachable {
            // Safety: `page.data_mut()` ensures the page is a unique copy of the underlying data.
            self.tlb.insert_write(page_start, unsafe { page.write_ptr() });
        }

        Ok(())
    }

    #[cold]
    fn read_unaligned<const N: usize>(&mut self, addr: u64, perm: u8) -> MemResult<[u8; N]> {
        let mut value = [0; N];
        for (i, byte) in value.iter_mut().enumerate() {
            let addr = addr.wrapping_add(i as u64) & self.address_mask;
            *byte = self.read_u8(addr, perm)?;
        }
        Ok(value)
    }

    #[cold]
    fn write_unaligned<const N: usize>(
        &mut self,
        addr: u64,
        value: [u8; N],
        perm: u8,
    ) -> MemResult<()> {
        for (i, &byte) in value.iter().enumerate() {
            let addr = addr.wrapping_add(i as u64) & self.address_mask;
            self.write_u8(addr, byte, perm)?;
        }
        Ok(())
    }

    #[cold]
    pub fn read_tlb_miss<const N: usize>(&mut self, addr: u64, perm: u8) -> MemResult<[u8; N]> {
        if !physical::is_aligned::<N>(addr) {
            return self.read_unaligned(addr, perm);
        }

        if perm != perm::NONE && ENABLE_MEMORY_HOOKS && !self.read_hooks.hooks.is_empty() {
            let mut hooks = std::mem::take(&mut self.read_hooks.hooks);
            for hook in &mut hooks {
                if let Some(handler) = hook.handler.as_mut() {
                    if hook.start <= addr && addr < hook.end {
                        if let Some(result) = handler.read(self, addr, N as u8) {
                            let mut buf = [0; N];
                            buf.copy_from_slice(&result.to_le_bytes()[..N]);
                            self.read_hooks.hooks = hooks;
                            return Ok(buf);
                        }
                    }
                }
            }
            debug_assert!(self.read_hooks.hooks.is_empty());
            self.read_hooks.hooks = hooks;
        }

        macro_rules! handle_io {
            ($id:expr) => {
                (|| {
                    let mut buf = [0; N];
                    self.io[$id].read(addr, &mut buf)?;
                    Ok(buf)
                })()
            };
        }

        let result = match self.last_io_handler.as_ref() {
            Some((start, end, id)) if (*start..=*end).contains(&addr) => {
                handle_io!(id.0)
            }
            _ => {
                tracing::trace!("read_tlb_miss: {:#0x}", self.page_aligned(addr));
                self.tlb_miss_count += 1;
                match self.mapping.get_with_range(addr).ok_or(MemError::Unmapped)? {
                    (_, _, MemoryMapping::Physical(entry)) => {
                        self.read_physical(entry.index, addr, perm)
                    }
                    (_, _, &MemoryMapping::Unallocated(entry)) => {
                        perm::check(entry.perm | perm::MAP, perm)?;
                        let index = self.init_physical(addr, false).ok_or(MemError::OutOfMemory)?;
                        self.read_physical(index, addr, perm)
                    }
                    (start, end, MemoryMapping::Io(id)) => {
                        self.last_io_handler = Some((start, end, IoHandler(*id)));
                        handle_io!(*id)
                    }
                }
            }
        };

        // Since we allow byte-level memory memory mapping to be created, rarely we may have a read
        // that crosses a mapping boundary which will result in a `Unmapped` error. To handle this
        // case try again using `read_unaligned` which will read one byte at a time.
        if N != 1 && result == Err(MemError::Unmapped) {
            return self.read_unaligned(addr, perm);
        }

        if let Ok(value) = result {
            if perm != perm::NONE && ENABLE_MEMORY_HOOKS {
                active_hooks!(addr, N, self.read_after_hooks, |hook: &mut dyn ReadAfterHook| {
                    hook.read(self, addr, &value)
                })
            }
        }

        result
    }

    #[cold]
    pub fn write_tlb_miss<const N: usize>(
        &mut self,
        addr: u64,
        value: [u8; N],
        perm: u8,
    ) -> MemResult<()> {
        if !physical::is_aligned::<N>(addr) {
            return self.write_unaligned(addr, value, perm);
        }

        tracing::trace!("write_tlb_miss: {:#0x}", self.page_aligned(addr));
        self.tlb_miss_count += 1;
        let mut legacy_notify = false;
        let result = (|| {
            let result = match self.mapping.get(addr).ok_or(MemError::Unmapped)? {
                MemoryMapping::Physical(entry) => self.write_physical(entry.index, addr, value, perm),
                &MemoryMapping::Unallocated(entry) => {
                    perm::check(entry.perm | perm::MAP, perm)?;
                    let index = self.init_physical(addr, true).ok_or(MemError::OutOfMemory)?;
                    self.write_physical(index, addr, value, perm)
                }
                MemoryMapping::Io(id) => self.io[*id].write(addr, &value),
            };
            legacy_notify = true;
            result
        })();

        // Handle case where we are writing across a mapping boundary (see `read_tlb_miss`).
        if legacy_notify && N != 1 && result == Err(MemError::Unmapped) {
            return self.write_unaligned(addr, value, perm);
        }

        self.observe_write(addr, &value, perm, result, legacy_notify);
        result
    }

    fn observe_write(&mut self, addr: u64, value: &[u8], perm: u8, result: MemResult<()>, legacy_notify: bool) {
        if perm != perm::NONE && ENABLE_MEMORY_HOOKS {
            active_hooks!(addr, value.len(), self.write_hooks, |hook: &mut dyn WriteHook| {
                // Keep the legacy post-attempt callback contract; richer observations are additive.
                if legacy_notify {
                    hook.write(self, addr, value);
                }
                hook.write_result(self, addr, value, result);
            })
        }
    }

    pub fn observe_external_write(&mut self, addr: u64, value: &[u8], result: MemResult<()>) {
        if ENABLE_MEMORY_HOOKS {
            active_hooks!(addr, value.len(), self.write_hooks, |hook: &mut dyn WriteHook| {
                hook.external_write_result(self, addr, value, result);
            })
        }
    }

    /// Get a reference to the virtual address space's mapping.
    pub fn get_mapping(&self) -> &VirtualMemoryMap {
        &self.mapping
    }

    /// Get a mutable reference to the virtual address space's mapping.
    pub fn get_mapping_mut(&mut self) -> &mut VirtualMemoryMap {
        &mut self.mapping
    }

    #[inline(always)]
    pub fn read<const N: usize>(&mut self, addr: u64, perm: u8) -> MemResult<[u8; N]> {
        match unsafe { self.tlb.read(addr, perm) } {
            Err(MemError::Unmapped) => self.read_tlb_miss(addr, perm),
            Err(MemError::Unaligned) if N != 1 => self.read_unaligned(addr, perm),
            x => x,
        }
    }

    #[inline(always)]
    pub fn write<const N: usize>(&mut self, addr: u64, value: [u8; N], perm: u8) -> MemResult<()> {
        match unsafe { self.tlb.write(addr, value, perm) } {
            Err(MemError::Unmapped) => self.write_tlb_miss(addr, value, perm),
            Err(MemError::Unaligned) if N != 1 => self.write_unaligned(addr, value, perm),
            x => x,
        }
    }

    pub fn read_cstr(&mut self, mut addr: u64, buf: &mut Vec<u8>) -> MemResult<u64> {
        loop {
            match self.read_u8(addr, perm::READ)? {
                0 => break,
                x => buf.push(x),
            }
            addr += 1;
        }
        Ok(addr)
    }
}

#[cold]
fn check_self_modifying_memset(page: &PageData, start: u64, len: u64, value: u8) -> MemResult<()> {
    let offset = PageData::offset(start);
    for i in offset..offset + len as usize {
        if page.perm[i] & perm::IN_CODE_CACHE != 0 && page.data[i] != value {
            let addr = start + (i - offset) as u64;
            tracing::error!("Self modifying code detected at {addr:#x}. Currently unsupported.");
            return Err(MemError::SelfModifyingCode);
        }
    }
    Ok(())
}

#[cold]
fn check_self_modifying_write(page: &PageData, addr: u64, value: &[u8]) -> MemResult<()> {
    let offset = PageData::offset(addr);
    for (i, ((old, perm), new)) in
        page.data[offset..].iter().zip(&page.perm[offset..]).zip(value).enumerate()
    {
        if perm & perm::IN_CODE_CACHE != 0 && *old != *new {
            tracing::error!(
                "Self modifying code detected at {:#x}. Currently unsupported.",
                addr + i as u64
            );
            return Err(MemError::SelfModifyingCode);
        }
    }
    Ok(())
}

macro_rules! impl_read_write {
    ($read_name:ident, $write_name:ident, $ty:ty) => {
        impl Mmu {
            #[inline(always)]
            pub fn $read_name(&mut self, addr: u64, perm: u8) -> MemResult<$ty> {
                Ok(<$ty>::from_le_bytes(self.read(addr, perm)?))
            }

            #[inline(always)]
            pub fn $write_name(&mut self, addr: u64, value: $ty, perm: u8) -> MemResult<()> {
                self.write(addr, value.to_le_bytes(), perm)
            }
        }
    };
}

impl_read_write!(read_u8, write_u8, u8);
impl_read_write!(read_u16, write_u16, u16);
impl_read_write!(read_u32, write_u32, u32);
impl_read_write!(read_u64, write_u64, u64);

#[cfg(test)]
mod smp_code_epoch_contract_tests {
    use super::*;
    use std::sync::atomic::Ordering;

    #[test]
    fn promotion_to_exec_makes_tlb_writes_advance_shared_epoch() {
        const ADDRESS: u64 = 0x6000;
        let mut mem = Mmu::default();
        assert!(mem.map_smp_shared_fresh(ADDRESS, perm::READ | perm::WRITE));
        let shared = mem.share_page(ADDRESS).unwrap();
        let initially_tracked = std::env::var("SOGEN_SMP_CODE_EPOCH_ONLY").as_deref() == Ok("0");
        assert_eq!(shared.smp_code_seen.load(Ordering::Acquire), u8::from(initially_tracked));

        mem.update_perm(ADDRESS, 0x1000, perm::READ | perm::WRITE | perm::EXEC).unwrap();
        assert_eq!(shared.smp_code_seen.load(Ordering::Acquire), 1);
        let before = shared.code_epoch();
        let index = mem.get_physical_index(ADDRESS).unwrap();
        let mut write_ptr = unsafe { mem.physical.get_mut(index).write_ptr() };
        unsafe { write_ptr.write(ADDRESS, [0x90_u8], perm::WRITE) }.unwrap();
        assert_eq!(shared.code_epoch(), before + 1);

        mem.update_perm(ADDRESS, 0x1000, perm::READ | perm::WRITE).unwrap();
        assert_eq!(shared.smp_code_seen.load(Ordering::Acquire), 1);
    }

    #[test]
    fn executable_mapping_tracks_write_before_first_decode() {
        const ADDRESS: u64 = 0x7000;
        let mut mem = Mmu::default();
        assert!(mem.map_smp_shared_fresh(ADDRESS, perm::READ | perm::EXEC));
        let shared = mem.share_page(ADDRESS).unwrap();
        assert_eq!(shared.smp_code_seen.load(Ordering::Acquire), 1);
        mem.write(ADDRESS, [0x90_u8], perm::NONE).unwrap();
        assert_eq!(shared.code_epoch(), 1);
        let index = mem.get_physical_index(ADDRESS).unwrap();
        assert!(!mem.get_physical(index).executed);
    }

    #[test]
    fn successful_ensure_exec_reasserts_marker() {
        const ADDRESS: u64 = 0x8000;
        let mut mem = Mmu::default();
        assert!(mem.map_smp_shared_fresh(ADDRESS, perm::READ | perm::WRITE | perm::EXEC));
        let shared = mem.share_page(ADDRESS).unwrap();
        shared.smp_code_seen.store(0, Ordering::Release);
        assert!(mem.ensure_executable(ADDRESS, 1));
        assert_eq!(shared.smp_code_seen.load(Ordering::Acquire), 1);
    }
}

#[cfg(test)]
mod smp_batch_tests {
    use super::*;

    #[test]
    fn captured_batch_keeps_noncontiguous_neighbors_and_shares_page_bytes() {
        let mut source = Mmu::default();
        let mut peer = Mmu::default();
        for address in [0x4000, 0x5000, 0x6000] {
            assert!(source.map_smp_shared_fresh(address, perm::READ | perm::WRITE));
        }
        for address in [0x2000, 0x9000] {
            assert!(peer.map_smp_shared_fresh(address, perm::READ | perm::WRITE));
        }
        // Warm an old translation, then replace it via the captured batch.
        assert!(peer.map_smp_shared_fresh(0x4000, perm::READ | perm::WRITE));
        peer.write(0x4000, [0x77_u8], perm::WRITE).unwrap();
        assert_eq!(peer.read_u8(0x4000, perm::READ).unwrap(), 0x77);
        assert!(peer.unmap_memory_len(0x4000, 0x1000));

        let captured = [0x4000, 0x5000, 0x6000]
            .map(|address| source.share_page(address).unwrap());
        assert!(peer.map_smp_shared_pages(0x4000, &captured));
        assert_eq!(peer.mapping.len(), 5);
        assert!(std::sync::Arc::ptr_eq(&peer.share_page(0x5000).unwrap(), &captured[1]));
        assert_eq!(peer.read_u8(0x4000, perm::READ).unwrap(), 0);
        source.write(0x5001, [0x5a_u8], perm::WRITE).unwrap();
        assert_eq!(peer.read_u8(0x5001, perm::READ).unwrap(), 0x5a);
        peer.write(0x6002, [0xa5_u8], perm::WRITE).unwrap();
        assert_eq!(source.read_u8(0x6002, perm::READ).unwrap(), 0xa5);
        assert!(peer.share_page(0x2000).is_some());
        assert!(peer.share_page(0x9000).is_some());
        assert!(peer.share_page(0x7000).is_none());
    }

    #[test]
    fn captured_batch_overlap_preflight_preserves_existing_map() {
        let mut source = Mmu::default();
        let mut peer = Mmu::default();
        for address in [0x4000, 0x5000, 0x6000] {
            assert!(source.map_smp_shared_fresh(address, perm::READ | perm::WRITE));
        }
        assert!(peer.map_smp_shared_fresh(0x5000, perm::READ | perm::WRITE));
        let existing = peer.share_page(0x5000).unwrap();
        let captured = [0x4000, 0x5000, 0x6000]
            .map(|address| source.share_page(address).unwrap());

        assert!(!peer.map_smp_shared_pages(0x4000, &captured));
        assert!(peer.share_page(0x4000).is_none());
        assert!(std::sync::Arc::ptr_eq(&peer.share_page(0x5000).unwrap(), &existing));
        assert!(peer.share_page(0x6000).is_none());
    }

    #[test]
    fn captured_batch_capacity_failure_retains_successful_prefix() {
        let mut source = Mmu::default();
        let mut peer = Mmu::default();
        for address in [0x4000, 0x5000, 0x6000] {
            assert!(source.map_smp_shared_fresh(address, perm::READ | perm::WRITE));
        }
        // Two built-in zero pages plus one existing mapping leave only two new slots.
        assert!(peer.set_capacity(5));
        assert!(peer.map_smp_shared_fresh(0x9000, perm::READ | perm::WRITE));
        peer.write(0x9000, [0x51_u8], perm::WRITE).unwrap();
        assert_eq!(peer.read_u8(0x9000, perm::READ).unwrap(), 0x51);
        let captured = [0x4000, 0x5000, 0x6000]
            .map(|address| source.share_page(address).unwrap());

        assert!(!peer.map_smp_shared_pages(0x4000, &captured));
        assert_eq!(peer.mapping.len(), 3);
        assert!(std::sync::Arc::ptr_eq(&peer.share_page(0x4000).unwrap(), &captured[0]));
        assert!(std::sync::Arc::ptr_eq(&peer.share_page(0x5000).unwrap(), &captured[1]));
        assert!(peer.share_page(0x6000).is_none());
        assert_eq!(peer.read_u8(0x9000, perm::READ).unwrap(), 0x51);
        source.write(0x5000, [0xa5_u8], perm::WRITE).unwrap();
        assert_eq!(peer.read_u8(0x5000, perm::READ).unwrap(), 0xa5);
    }

    #[test]
    fn fresh_batch_creates_distinct_shared_pages_between_existing_regions() {
        let mut source = Mmu::default();
        assert!(source.map_smp_shared_fresh(0x2000, perm::READ | perm::WRITE));
        assert!(source.map_smp_shared_fresh(0x9000, perm::READ | perm::WRITE));
        assert!(source.map_smp_shared_fresh_pages(0x4000, 3, perm::READ | perm::WRITE));
        assert_eq!(source.mapping.len(), 5);
        let first = source.share_page(0x4000).unwrap();
        let second = source.share_page(0x5000).unwrap();
        assert!(!std::sync::Arc::ptr_eq(&first, &second));
        source.write(0x4000, [0x13_u8], perm::WRITE).unwrap();
        assert_eq!(source.read_u8(0x4000, perm::READ).unwrap(), 0x13);
        assert_eq!(source.read_u8(0x5000, perm::READ).unwrap(), 0);
        assert!(!source.map_smp_shared_fresh_pages(0x5000, 2, perm::READ));
        assert_eq!(source.mapping.len(), 5);
    }

    #[test]
    #[ignore = "manual Release microbenchmark; does not assert a machine-dependent speed ratio"]
    fn captured_batch_middle_insert_benchmark() {
        use std::time::Instant;

        const EXISTING: u64 = 4096;
        const CAPTURED: u64 = 2131;
        const HIGH: u64 = 0x8000_0000;
        const MIDDLE: u64 = 0x4000_0000;
        let pages = (0..CAPTURED)
            .map(|_| std::sync::Arc::new(PageData::default()))
            .collect::<Vec<_>>();
        let prepare = || {
            let mut mem = Mmu::default();
            for i in 0..EXISTING {
                assert!(mem.map_smp_shared_fresh(HIGH + i * 0x1000, perm::READ | perm::WRITE));
            }
            mem
        };

        let mut serial = prepare();
        let started = Instant::now();
        for (i, page) in pages.iter().enumerate() {
            assert!(serial.map_smp_shared(MIDDLE + i as u64 * 0x1000, page.clone()));
        }
        let serial_time = started.elapsed();

        let mut batch = prepare();
        let started = Instant::now();
        assert!(batch.map_smp_shared_pages(MIDDLE, &pages));
        let batch_time = started.elapsed();
        assert_eq!(serial.mapping.len(), batch.mapping.len());
        assert_eq!(batch.mapping.len(), (EXISTING + CAPTURED) as usize);
        eprintln!("middle insert: existing={EXISTING} captured={CAPTURED} mappings={} serial={serial_time:?} batch={batch_time:?}", batch.mapping.len());
    }
}

#[cfg(test)]
mod watchpoint_tests {
    use super::*;
    use crate::Mapping;
    use std::{cell::RefCell, rc::Rc};

    #[derive(Debug, PartialEq)]
    struct Attempt {
        address: u64,
        value: Vec<u8>,
        result: MemResult<()>,
        after: Option<Vec<u8>>,
    }

    struct Observer(Rc<RefCell<Vec<Attempt>>>);

    impl WriteHook for Observer {
        fn write(&mut self, _mem: &mut Mmu, _addr: u64, _value: &[u8]) {}

        fn write_result(&mut self, mem: &mut Mmu, addr: u64, value: &[u8], result: MemResult<()>) {
            let mut after = vec![0; value.len()];
            let readable = mem.read_bytes(addr, &mut after, perm::NONE).is_ok();
            self.0.borrow_mut().push(Attempt {
                address: addr,
                value: value.to_vec(),
                result,
                after: readable.then_some(after),
            });
        }
    }

    #[test]
    fn overlap_is_half_open_without_endpoint_overflow() {
        assert!(access_overlaps(0x1008, 0x100c, 0x1000, 16));
        assert!(access_overlaps(0x1008, 0x100c, 0x100b, 1));
        assert!(!access_overlaps(0x1008, 0x100c, 0x1000, 8));
        assert!(!access_overlaps(0x1008, 0x100c, 0x100c, 1));
        assert!(!access_overlaps(0x1008, 0x100c, 0x1008, 0));
        assert!(!access_overlaps(8, 4, 0, 16));
        assert!(access_overlaps(u64::MAX - 3, u64::MAX, u64::MAX - 7, 16));
        assert!(!access_overlaps(0, 8, u64::MAX - 7, 16));
        let hook: HookEntry<dyn WriteHook> = HookEntry {
            start: u64::MAX - 3, end: u64::MAX, handler: Some(Box::new(())),
        };
        assert_eq!(*hook.range(0x1000).end(), u64::MAX);
        let mut mmu = Mmu::default();
        assert!(mmu.add_write_hook(5, 5, Box::new(())).is_none());
        assert!(mmu.add_write_hook(u64::MAX - 3, 4, Box::new(())).is_none());
        assert!(mmu.add_read_hook(8, 4, Box::new(())).is_none());
    }

    #[test]
    fn enclosing_write_is_observed_after_commit_even_after_tlb_warmup() {
        let mut mmu = Mmu::default();
        assert!(mmu.map_memory_len(0x1000, 0x2000, Mapping { perm: perm::READ | perm::WRITE, value: 0 }));
        mmu.write(0x1000, [0; 16], perm::WRITE).unwrap();
        let attempts = Rc::new(RefCell::new(vec![]));
        mmu.add_write_hook(0x1008, 0x100c, Box::new(Observer(attempts.clone()))).unwrap();
        // An unrelated access on the same watched page must not restore a bypassing write TLB entry.
        mmu.write(0x1100, [1; 16], perm::WRITE).unwrap();
        mmu.write(0x1000, [2; 16], perm::WRITE).unwrap();
        mmu.write(0x1000, [3; 16], perm::WRITE).unwrap();
        mmu.write(0x1000, [4; 8], perm::WRITE).unwrap();
        mmu.write(0x100c, [5; 4], perm::WRITE).unwrap();
        let observed = attempts.borrow();
        assert_eq!(observed.len(), 2);
        for (index, attempt) in observed.iter().enumerate() {
            assert_eq!(attempt.address, 0x1000);
            assert_eq!(attempt.result, Ok(()));
            assert_eq!(attempt.value, vec![(index + 2) as u8; 16]);
            assert_eq!(attempt.after.as_ref(), Some(&attempt.value));
        }
    }

    #[test]
    fn rejected_and_partial_writes_keep_individual_results() {
        let mut mmu = Mmu::default();
        assert!(mmu.map_memory_len(0x1000, 0x1000, Mapping { perm: perm::READ, value: 0xAA }));
        let attempts = Rc::new(RefCell::new(vec![]));
        mmu.add_write_hook(0x1000, 0x3000, Box::new(Observer(attempts.clone()))).unwrap();
        assert_eq!(mmu.write(0x1000, [0xBB; 4], perm::WRITE), Err(MemError::WriteViolation));
        assert_eq!(mmu.write(0x2000, [0xCC; 4], perm::WRITE), Err(MemError::Unmapped));
        mmu.update_perm(0x1000, 0x1000, perm::READ | perm::WRITE).unwrap();
        assert_eq!(mmu.write(0x1fff, [0xDD, 0xEE], perm::WRITE), Err(MemError::Unmapped));
        let observed = attempts.borrow();
        assert_eq!(observed.len(), 4);
        assert_eq!(observed[0].result, Err(MemError::WriteViolation));
        assert_eq!(observed[0].after, Some(vec![0xAA; 4]));
        assert_eq!(observed[1].result, Err(MemError::Unmapped));
        assert_eq!(observed[1].after, None);
        assert_eq!(observed[2].address, 0x1fff);
        assert_eq!(observed[2].result, Ok(()));
        assert_eq!(observed[2].after, Some(vec![0xDD]));
        assert_eq!(observed[3].address, 0x2000);
        assert_eq!(observed[3].value, vec![0xEE]);
        assert_eq!(observed[3].result, Err(MemError::Unmapped));
    }

    #[test]
    fn after_read_overlap_and_external_writes_have_explicit_scope() {
        struct ReadObserver(Rc<RefCell<Vec<(u64, Vec<u8>)>>>);
        impl ReadAfterHook for ReadObserver {
            fn read(&mut self, _mem: &mut Mmu, addr: u64, value: &[u8]) {
                self.0.borrow_mut().push((addr, value.to_vec()));
            }
        }
        let mut mmu = Mmu::default();
        assert!(mmu.map_memory_len(0x1000, 0x1000, Mapping { perm: perm::READ | perm::WRITE, value: 0 }));
        let writes = Rc::new(RefCell::new(vec![]));
        let reads = Rc::new(RefCell::new(vec![]));
        mmu.add_write_hook(0x1008, 0x100c, Box::new(Observer(writes.clone()))).unwrap();
        mmu.add_read_after_hook(0x1008, 0x100c, Box::new(ReadObserver(reads.clone()))).unwrap();
        mmu.write(0x1000, [9; 16], perm::NONE).unwrap();
        assert_eq!(mmu.read::<16>(0x1000, perm::READ).unwrap(), [9; 16]);
        assert!(writes.borrow().is_empty());
        assert_eq!(*reads.borrow(), vec![(0x1000, vec![9; 16])]);

        let external = [7; 16];
        let result = mmu.write(0x1000, external, perm::NONE);
        mmu.observe_external_write(0x1000, &external, result);
        let observed = writes.borrow();
        assert_eq!(observed.len(), 1);
        assert_eq!(observed[0].address, 0x1000);
        assert_eq!(observed[0].value, external);
        assert_eq!(observed[0].after, Some(external.to_vec()));
    }
}
