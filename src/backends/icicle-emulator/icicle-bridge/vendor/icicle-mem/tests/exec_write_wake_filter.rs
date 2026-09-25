use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};

use icicle_mem::{Mmu, perm, physical::ExecWriteWake};

const PAGE: u64 = 0x4000;

fn fixture() -> (Mmu, Arc<icicle_mem::physical::PageData>, Arc<ExecWriteWake>, [Arc<AtomicBool>; 2])
{
    let pending = [Arc::new(AtomicBool::new(false)), Arc::new(AtomicBool::new(false))];
    let wake = Arc::new(ExecWriteWake::new(
        pending.iter().map(|flag| (Arc::clone(flag), Arc::new(AtomicBool::new(false)))).collect(),
    ));
    let mut mem = Mmu::default();
    mem.set_exec_write_wake(Arc::clone(&wake));
    assert!(mem.map_smp_shared_fresh(PAGE, perm::READ | perm::WRITE | perm::EXEC));
    let shared = mem.share_page(PAGE).unwrap();
    (mem, shared, wake, pending)
}

#[test]
fn serial_run_filters_unrelated_bytes_but_wakes_for_cached_code() {
    let (mut mem, shared, wake, pending) = fixture();
    mem.write(PAGE, [0x90], perm::WRITE).unwrap();
    assert!(mem.ensure_executable(PAGE, 1));

    wake.enter_vm();
    assert!(shared.overlaps_exec_code_slots(PAGE, 1));
    assert!(!shared.overlaps_exec_code_slots(PAGE + 0x80, 1));
    mem.write(PAGE + 0x80, [1], perm::WRITE).unwrap();
    let index = mem.get_physical_index(PAGE).unwrap();
    let mut write_ptr = unsafe { mem.get_physical_mut(index).write_ptr() };
    unsafe { write_ptr.write(PAGE + 0x81, [2], perm::WRITE) }.unwrap();
    shared.notify_exec_write_jit(PAGE + 0x80, 1);
    assert_eq!(wake.filter_counts(), (3, 0, 0));
    assert_eq!(wake.write_counts(), (0, 0, 0));
    assert!(pending.iter().all(|flag| !flag.load(Ordering::Acquire)));

    shared.notify_exec_write_jit(PAGE, 1);
    assert_eq!(wake.filter_counts(), (3, 1, 0));
    assert_eq!(wake.write_counts(), (1, 0, 0));
    assert!(pending.iter().all(|flag| flag.load(Ordering::Acquire)));
    wake.leave_vm();
}

#[test]
fn overlapping_lifts_keep_conservative_wake_before_code_is_marked() {
    let (_mem, shared, wake, pending) = fixture();
    assert!(!shared.overlaps_exec_code_slots(PAGE, 1));
    wake.enter_vm();
    wake.enter_vm();
    shared.notify_exec_write_jit(PAGE, 1);
    assert_eq!(wake.filter_counts(), (0, 0, 1));
    assert_eq!(wake.write_counts(), (1, 0, 0));
    assert!(pending.iter().all(|flag| flag.load(Ordering::Acquire)));
    wake.leave_vm();
    wake.leave_vm();
}

#[test]
fn lone_first_lift_wakes_before_code_slots_are_published() {
    let (mut mem, shared, wake, pending) = fixture();
    assert!(!shared.overlaps_exec_code_slots(PAGE, 1));
    wake.enter_vm();
    mem.write(PAGE, [0x90], perm::WRITE).unwrap();
    assert_eq!(wake.write_counts(), (0, 1, 0));
    assert!(pending.iter().all(|flag| flag.load(Ordering::Acquire)));
    wake.leave_vm();
}

#[test]
fn code_slot_marking_covers_instruction_boundary_and_remap() {
    let (mut mem, shared, wake, _pending) = fixture();
    mem.write(PAGE + 15, [0x90], perm::WRITE).unwrap();
    mem.write(PAGE + 16, [0x90], perm::WRITE).unwrap();
    assert!(mem.ensure_executable(PAGE + 15, 2));
    assert!(shared.overlaps_exec_code_slots(PAGE + 15, 1));
    assert!(shared.overlaps_exec_code_slots(PAGE + 16, 1));
    assert!(!shared.overlaps_exec_code_slots(PAGE + 32, 1));

    assert!(mem.unmap_memory_len(PAGE, 0x1000));
    assert!(mem.map_smp_shared(PAGE, Arc::clone(&shared)));
    wake.enter_vm();
    shared.notify_exec_write_jit(PAGE + 16, 1);
    assert_eq!(wake.filter_counts(), (0, 1, 0));
    wake.leave_vm();
}

#[test]
fn unexpected_cross_page_jit_range_notifies_conservatively() {
    let (_mem, shared, wake, pending) = fixture();
    wake.enter_vm();
    shared.notify_exec_write_jit(PAGE + 0xfff, 2);
    assert_eq!(wake.filter_counts(), (0, 1, 0));
    assert!(pending.iter().all(|flag| flag.load(Ordering::Acquire)));
    wake.leave_vm();
}
