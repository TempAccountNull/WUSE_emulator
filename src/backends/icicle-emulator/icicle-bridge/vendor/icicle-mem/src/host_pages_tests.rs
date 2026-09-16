use crate::{Mapping, MemError, Mmu, perm};
use crate::physical::{PageData, PAGE_SIZE};
use std::panic::{catch_unwind, AssertUnwindSafe};

const A: u64 = 0x10000;
const B: u64 = 0x20000;
const RW: u8 = perm::READ | perm::WRITE;

#[repr(C, align(4096))]
struct Host([u8; 2 * PAGE_SIZE]);

fn host() -> Box<Host> { Box::new(Host([0; 2 * PAGE_SIZE])) }

fn map(mem: &mut Mmu, host: &mut Host) {
    assert!(unsafe { mem.map_host_memory(A, host.0.as_mut_ptr(), host.0.len() as u64, RW) });
}

fn access<const N: usize>(mem: &mut Mmu, host: &mut Host, offset: usize) {
    for value in [0x17, 0xc9, 0x3a] {
        host.0[offset..offset + N].fill(value);
        assert_eq!(mem.read::<N>(A + offset as u64, perm::READ).unwrap(), [value; N]);
        mem.write(A + offset as u64, [value ^ 0xff; N], perm::WRITE).unwrap();
        assert_eq!(&host.0[offset..offset + N], &[value ^ 0xff; N]);
    }
}

#[test]
fn host_pages_live_bytes_all_widths_and_page_crossings() {
    let mut host = host();
    let mut mem = Mmu::default();
    map(&mut mem, &mut host);
    for offset in [0, 1, 0xff9, 0xfff, 0x1000] {
        access::<1>(&mut mem, &mut host, offset);
        access::<2>(&mut mem, &mut host, offset);
        access::<4>(&mut mem, &mut host, offset);
        access::<8>(&mut mem, &mut host, offset);
        access::<16>(&mut mem, &mut host, offset);
    }
    for address in [A, A + PAGE_SIZE as u64] {
        assert!(unsafe { mem.tlb.read::<1>(address, perm::READ) }.is_err());
        assert!(unsafe { mem.tlb.write::<1>(address, [0], perm::WRITE) }.is_err());
    }
}

#[test]
fn host_pages_alias_permissions_and_original_unmap() {
    let mut host = host();
    let mut mem = Mmu::default();
    map(&mut mem, &mut host);
    mem.map_shared(B, A, PAGE_SIZE as u64, RW).unwrap();
    mem.update_perm(B, PAGE_SIZE as u64, perm::READ).unwrap();
    assert_eq!(mem.write_u32(B, 1, perm::WRITE), Err(MemError::WriteViolation));
    mem.write_u32(A, 123, perm::WRITE).unwrap();
    assert_eq!(mem.read_u32(B, perm::READ).unwrap(), 123);
    assert!(mem.unmap_memory_len(A, 2 * PAGE_SIZE as u64));
    assert!(mem.has_host_mappings());
    host.0[0..4].copy_from_slice(&456u32.to_le_bytes());
    assert_eq!(mem.read_u32(B, perm::READ).unwrap(), 456);
    assert_eq!(mem.host_mapping_aliases(host.0.as_ptr() as usize, 4), vec![(B, PAGE_SIZE as u64)]);
    assert!(mem.unmap_memory_len(B, PAGE_SIZE as u64));
    assert!(!mem.has_host_mappings());
}

#[test]
fn host_pages_partial_holes_and_protections_keep_live_fragments() {
    let mut host = host();
    let mut mem = Mmu::default();
    map(&mut mem, &mut host);
    mem.update_perm(A + 3, 2, perm::READ).unwrap();
    assert_eq!(mem.write_u64(A, u64::MAX, perm::WRITE), Err(MemError::WriteViolation));
    assert_eq!(host.0[3], 0);
    assert!(mem.unmap_memory_len(A + 3, 2));
    assert_eq!(mem.read_u64(A, perm::READ), Err(MemError::Unmapped));
    assert!(mem.map_memory_len(A + 3, 2, Mapping { perm: RW | perm::INIT, value: 0x55 }));
    assert_eq!(mem.read::<2>(A + 3, perm::READ).unwrap(), [0x55; 2]);
    host.0[..3].fill(0x11);
    host.0[5..8].fill(0x22);
    assert_eq!(mem.read::<8>(A, perm::READ).unwrap(), [0x11, 0x11, 0x11, 0x55, 0x55, 0x22, 0x22, 0x22]);
    mem.write::<8>(A, [0x33; 8], perm::WRITE).unwrap();
    assert_eq!(&host.0[..8], &[0x33, 0x33, 0x33, 0, 0, 0x33, 0x33, 0x33]);
    assert_eq!(mem.read_u8(A + 3, perm::READ).unwrap(), 0x33);
}

#[test]
fn host_pages_mapping_validation_and_capacity_rollback() {
    let mut host = host();
    let ptr = host.0.as_mut_ptr();
    let mut mem = Mmu::default();
    for (address, pointer, length) in [
        (A + 1, ptr, 4096), (A, unsafe { ptr.add(1) }, 4096),
        (A, ptr, 0), (A, ptr, 4095), (A, std::ptr::null_mut(), 4096),
        (0xfffffffffffff000, ptr, 8192), (A, ptr, 1u64 << 63),
    ] {
        assert!(!unsafe { mem.map_host_memory(address, pointer, length, RW) });
    }
    assert_eq!(mem.total_pages(), 2);
    assert!(mem.set_capacity(3));
    assert!(!unsafe { mem.map_host_memory(A, ptr, 8192, RW) });
    assert_eq!(mem.total_pages(), 2);
    assert!(!mem.has_host_mappings());
    assert_eq!(mem.read_u8(A, perm::READ), Err(MemError::Unmapped));
    assert!(unsafe { mem.map_host_memory(A, ptr, 4096, RW) });
    assert!(!unsafe { mem.map_host_memory(A, ptr, 4096, RW) });
    assert_eq!(mem.total_pages(), 3);
}

#[test]
fn host_pages_snapshots_rejected_until_last_alias_removed() {
    let mut host = host();
    let mut mem = Mmu::default();
    map(&mut mem, &mut host);
    mem.map_shared(B, A, 4096, RW).unwrap();
    assert!(catch_unwind(AssertUnwindSafe(|| mem.snapshot())).is_err());
    assert!(catch_unwind(AssertUnwindSafe(|| mem.snapshot_virtual_mapping())).is_err());
    assert!(mem.unmap_memory_len(A, 8192));
    assert!(catch_unwind(AssertUnwindSafe(|| mem.snapshot())).is_err());
    mem.write_u8(B, 0x77, perm::WRITE).unwrap();
    assert_eq!(host.0[0], 0x77);
    assert!(mem.unmap_memory_len(B, 4096));
    mem.snapshot();
    mem.snapshot_virtual_mapping();
}

#[test]
fn host_pages_dump_live_bytes_without_serializing_pointer() {
    let mut host = host();
    let mut mem = Mmu::default();
    map(&mut mem, &mut host);
    host.0[0] = 0xab;
    let index = mem.get_physical_index(A).unwrap();
    let bytes = mem.get_physical(index).data().as_bytes();
    assert_eq!(bytes.len(), 2 * PAGE_SIZE);
    assert_eq!(bytes[0], 0xab);
    let restored = PageData::from_bytes(&bytes);
    assert!(!restored.data.is_external());
    host.0[0] = 0xcd;
    assert_eq!(restored.data[0], 0xab);
    assert_eq!(mem.get_physical(index).data().data[0], 0xcd);
    assert_eq!(std::mem::offset_of!(PageData, data), 0);
    assert!(std::mem::offset_of!(PageData, perm) >= PAGE_SIZE);
    assert_eq!(std::mem::offset_of!(PageData, perm) % 16, 0);
}
