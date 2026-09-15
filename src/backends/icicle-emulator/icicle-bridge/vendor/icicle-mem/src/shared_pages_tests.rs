use crate::{Mapping, MemError, Mmu, perm};

const A: u64 = 0x10000;
const B: u64 = 0x20000;
const C: u64 = 0x30000;
const RW: u8 = perm::MAP | perm::INIT | perm::READ | perm::WRITE;

fn shared() -> Mmu {
    let mut mem = Mmu::default();
    assert!(mem.map_memory_len(A, 0x1000, Mapping { perm: RW, value: 0 }));
    mem.map_shared(B, A, 0x1000, RW).unwrap();
    mem.map_shared(C, A, 0x1000, RW).unwrap();
    mem
}

#[test]
fn shared_pages_coherent_after_cached_reads_and_snapshot() {
    let mut mem = shared();
    let snapshot = mem.snapshot();
    for i in 0..20 {
        assert_eq!(mem.read_u32(B, perm::READ).unwrap(), i);
        mem.write_u32(C, i + 1, perm::WRITE).unwrap();
        assert_eq!(mem.read_u32(A, perm::READ).unwrap(), i + 1);
    }
    mem.restore(snapshot);
    assert_eq!(mem.read_u32(C, perm::READ).unwrap(), 0);
    mem.write_u32(A, 99, perm::WRITE).unwrap();
    assert_eq!(mem.read_u32(B, perm::READ).unwrap(), 99);
}

#[test]
fn shared_pages_have_independent_permissions_and_unmap() {
    let mut mem = shared();
    mem.update_perm(B, 0x1000, perm::READ).unwrap();
    assert_eq!(mem.write_u32(B, 1, perm::WRITE), Err(MemError::WriteViolation));
    mem.write_u32(C, 2, perm::WRITE).unwrap();
    assert_eq!(mem.read_u32(B, perm::READ).unwrap(), 2);
    assert!(mem.unmap_memory_len(A, 0x1000));
    mem.write_u32(C, 3, perm::WRITE).unwrap();
    assert_eq!(mem.read_u32(B, perm::READ).unwrap(), 3);
    assert!(mem.unmap_memory_len(C, 0x1000));
    assert_eq!(mem.read_u32(B, perm::READ).unwrap(), 3);
    assert_eq!(mem.read_u32(C, perm::READ), Err(MemError::Unmapped));
}

#[test]
fn shared_pages_preserve_virtual_snapshot_aliases() {
    let mut mem = shared();
    let snapshot = mem.snapshot_virtual_mapping();
    mem.write_u32(B, 123, perm::WRITE).unwrap();
    assert_eq!(mem.read_u32(C, perm::READ).unwrap(), 123);
    mem.restore_virtual_mapping(snapshot);
    assert_eq!(mem.read_u32(B, perm::READ).unwrap(), 0);
    mem.write_u32(C, 456, perm::WRITE).unwrap();
    assert_eq!(mem.read_u32(A, perm::READ).unwrap(), 456);
}

#[test]
fn shared_pages_cross_view_boundary_and_execute_permissions() {
    let mut mem = Mmu::default();
    assert!(mem.map_memory_len(A, 0x1000, Mapping { perm: RW, value: 0 }));
    mem.map_shared(A + 0x1000, A, 0x1000, RW).unwrap();
    mem.write_u64(A + 0xffc, 0x123456789abcdef0, perm::WRITE).unwrap();
    assert_eq!(mem.read_u32(A, perm::READ).unwrap(), 0x12345678);
    assert_eq!(mem.read_u32(A + 0x1ffc, perm::READ).unwrap(), 0x9abcdef0);
    assert!(!mem.ensure_executable(A, 1));
    mem.update_perm(A, 0x1000, perm::READ | perm::EXEC).unwrap();
    assert!(mem.ensure_executable(A, 1));
    assert!(!mem.ensure_executable(A + 0x1000, 1));
}

#[test]
fn shared_pages_validate_without_partial_mapping() {
    let mut mem = shared();
    assert!(mem.map_shared(B, A, 0x1000, RW).is_err());
    assert!(mem.map_shared(0x40000, A, 0x2000, RW).is_err());
    assert_eq!(mem.read_u8(0x40000, perm::READ), Err(MemError::Unmapped));
    assert!(mem.map_shared(0x40001, A, 0x1000, RW).is_err());
}
