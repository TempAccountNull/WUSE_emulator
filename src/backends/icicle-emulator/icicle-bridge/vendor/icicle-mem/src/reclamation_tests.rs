use crate::{Mapping, MemoryMapping, Mmu, PhysicalMapping, perm};

const RW: u8 = perm::MAP | perm::INIT | perm::READ | perm::WRITE;
const A: u64 = 0x10000;
const B: u64 = 0x20000;

fn mapped(mem: &mut Mmu, address: u64, byte: u8) -> crate::physical::Index {
    assert!(mem.map_memory_len(address, 4096, Mapping { perm: RW, value: 0 }));
    mem.write_bytes(address, &[byte; 4096], perm::WRITE).unwrap();
    mem.get_physical_index(address).unwrap()
}

#[test]
fn repeated_unmap_reuses_bounded_pool() {
    let mut mem = Mmu::new();
    assert!(mem.set_capacity(16));
    for i in 0..1000 {
        let page = mapped(&mut mem, A, (i % 254 + 1) as u8);
        assert!(mem.unmap_memory_len(A, 4096));
        assert_eq!(mem.reclaim_unmapped_physical(&[page], &[]), 1);
        assert_eq!(mem.total_pages(), 2);
    }
}

#[test]
fn alias_pins_page_until_last_mapping_is_removed() {
    let mut mem = Mmu::new();
    let page = mapped(&mut mem, A, 0x42);
    assert!(mem.map_memory_len(
        B,
        4096,
        MemoryMapping::Physical(PhysicalMapping { addr: B, index: page })
    ));
    assert!(mem.unmap_memory_len(A, 4096));
    assert_eq!(mem.reclaim_unmapped_physical(&[page], &[]), 0);
    assert_eq!(mem.read::<1>(B, perm::READ), Ok([0x42]));
    assert!(mem.unmap_memory_len(B, 4096));
    assert_eq!(mem.reclaim_unmapped_physical(&[page, page], &[]), 1);
    assert_eq!(mem.total_pages(), 2);
}

#[test]
fn partial_mapping_pins_remaining_bytes() {
    let mut mem = Mmu::new();
    let page = mapped(&mut mem, A, 0x23);
    assert!(mem.unmap_memory_len(A, 2048));
    assert_eq!(mem.reclaim_unmapped_physical(&[page], &[]), 0);
    assert_eq!(mem.read::<1>(A + 2048, perm::READ), Ok([0x23]));
    assert!(mem.unmap_memory_len(A + 2048, 2048));
    assert_eq!(mem.reclaim_unmapped_physical(&[page], &[]), 1);
}

#[test]
fn saved_virtual_mapping_is_an_additional_root() {
    let mut mem = Mmu::new();
    let page = mapped(&mut mem, A, 0x39);
    let saved = mem.take_virtual_mapping();
    assert_eq!(mem.reclaim_unmapped_physical(&[page], &[&saved]), 0);
    mem.restore_virtual_mapping(saved);
    assert_eq!(mem.read::<1>(A, perm::READ), Ok([0x39]));
}

#[test]
fn snapshot_survives_slot_reuse_and_restores_free_list() {
    let mut mem = Mmu::new();
    assert!(mem.set_capacity(4));
    let original = mapped(&mut mem, A, 0x51);
    let saved = mem.snapshot();
    assert!(mem.unmap_memory_len(A, 4096));
    assert_eq!(mem.reclaim_unmapped_physical(&[original], &[]), 1);
    assert_eq!(mapped(&mut mem, B, 0x72), original);
    mem.restore(saved);
    assert_eq!(mem.read::<1>(A, perm::READ), Ok([0x51]));
    assert_ne!(mapped(&mut mem, B, 0x73), original);
    assert_eq!(mem.read::<1>(A, perm::READ), Ok([0x51]));
}

#[test]
fn cached_code_is_not_reclaimed() {
    let mut mem = Mmu::new();
    let page = mapped(&mut mem, A, 0x90);
    mem.get_physical_mut(page).executed = true;
    assert!(mem.unmap_memory_len(A, 4096));
    assert_eq!(mem.reclaim_unmapped_physical(&[page], &[]), 0);
    mem.get_physical_mut(page).executed = false;
    assert_eq!(mem.reclaim_unmapped_physical(&[page], &[]), 1);
}

#[test]
fn zero_pages_are_not_reclaimed() {
    let mut mem = Mmu::new();
    assert!(mem.map_memory_len(A, 4096, Mapping { perm: RW, value: 0 }));
    assert_eq!(mem.read::<1>(A, perm::READ), Ok([0]));
    let zero = mem.get_physical_index(A).unwrap();
    assert!(zero.is_zero_page());
    assert!(mem.unmap_memory_len(A, 4096));
    assert_eq!(mem.reclaim_unmapped_physical(&[zero, zero], &[]), 0);
    assert_eq!(mem.total_pages(), 2);
}

#[test]
fn reused_physical_slots_clear_bytes_permissions_and_flags() {
    let mut mem = crate::physical::PhysicalMemory::new(4);
    let page = mem.alloc().unwrap();
    let first = mem.get_mut(page);
    first.data_mut().data.fill(0x41);
    first.data_mut().perm.fill(RW | perm::IN_CODE_CACHE);
    first.modified = true;
    first.copy_on_write = true;
    let saved = mem.snapshot();
    mem.free(page);
    assert_eq!(mem.alloc(), Some(page));
    let reused = mem.get(page);
    assert!(reused.data().data.iter().all(|&x| x == 0));
    assert!(reused.data().perm.iter().all(|&x| x == 0));
    assert!(!reused.modified && !reused.copy_on_write && !reused.executed);
    assert!(saved.get(page).data().data.iter().all(|&x| x == 0x41));
}
