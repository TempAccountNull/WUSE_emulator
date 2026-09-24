mod packed_mul_high;
mod simd_minmax;
mod vector_operand;
mod aes;
mod aligned_move;
mod icicle;
mod packed_sad;
mod reciprocal_sqrt;
mod registers;
mod xstate;

use icicle::{IcicleEmulator, IcicleStopInfo};
use registers::X86Register;
use std::cell::RefCell;
use std::collections::HashMap;
use std::os::raw::c_void;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

fn stop_flags() -> &'static Mutex<HashMap<usize, Arc<AtomicBool>>> {
    static FLAGS: OnceLock<Mutex<HashMap<usize, Arc<AtomicBool>>>> = OnceLock::new();
    FLAGS.get_or_init(|| Mutex::new(HashMap::new()))
}

thread_local! {
    // An owner callback can also stop its execution hook on the next instruction.
    static RUNNING_STOP: RefCell<Option<(usize, Rc<RefCell<bool>>)>> = const { RefCell::new(None) };
}

struct RunningStopGuard;

impl RunningStopGuard {
    fn new(ptr: usize, stop: Rc<RefCell<bool>>) -> Self {
        RUNNING_STOP.with(|slot| {
            let mut current = slot.borrow_mut();
            assert!(current.is_none(), "nested Icicle execution on one host thread");
            *current = Some((ptr, stop));
        });
        Self
    }
}

impl Drop for RunningStopGuard {
    fn drop(&mut self) {
        RUNNING_STOP.with(|slot| *slot.borrow_mut() = None);
    }
}

fn to_cbool(value: bool) -> i32 {
    if value {
        return 1;
    }

    return 0;
}

#[unsafe(no_mangle)]
pub fn icicle_create_emulator(memory_limit_mib: u64) -> *mut c_void {
    let mut emulator = Box::new(IcicleEmulator::new());
    if memory_limit_mib != 0 && !emulator.set_memory_limit_mib(memory_limit_mib) {
        return std::ptr::null_mut();
    }
    let stop_flag = emulator.stop_flag();
    let ptr = Box::into_raw(emulator) as *mut c_void;
    stop_flags().lock().unwrap().insert(ptr as usize, stop_flag);
    return ptr;
}

#[unsafe(no_mangle)]
pub fn icicle_start(ptr: *mut c_void, count: usize) {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let _owner = RunningStopGuard::new(ptr as usize, emulator.owner_stop_cell());
        emulator.start(count as u64);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_get_stop_info(ptr: *mut c_void, out: *mut IcicleStopInfo) -> i32 {
    if out.is_null() {
        return 0;
    }

    unsafe {
        let emulator = &*(ptr as *mut IcicleEmulator);
        *out = emulator.last_stop_info();
    }

    return 1;
}

#[unsafe(no_mangle)]
pub fn icicle_get_icount(ptr: *mut c_void) -> u64 {
    unsafe {
        let emulator = &*(ptr as *mut IcicleEmulator);
        return emulator.icount();
    }
}

#[unsafe(no_mangle)]
pub fn icicle_invalidate_code_range(ptr: *mut c_void, address: u64, length: u64) -> i32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return if emulator.invalidate_code_range_public(address, length) { 1 } else { 0 };
    }
}

#[unsafe(no_mangle)]
pub fn icicle_perm_epoch_of_range(ptr: *mut c_void, address: u64, length: u64) -> u64 {
    unsafe {
        let emulator = &*(ptr as *mut IcicleEmulator);
        return emulator.perm_epoch_of_range(address, length);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_code_range_is_cached(ptr: *mut c_void, address: u64, length: u64) -> i32 {
    unsafe {
        let emulator = &*(ptr as *mut IcicleEmulator);
        return if emulator.code_range_is_cached(address, length) { 1 } else { 0 };
    }
}

#[unsafe(no_mangle)]
pub fn icicle_get_vm_exit_description(ptr: *mut c_void, callback: DataFunction, data: *mut c_void) {
    let emulator = unsafe { &*(ptr as *mut IcicleEmulator) };
    let description = emulator.vm_exit_description();
    callback(data, description.as_ptr() as *const c_void, description.len());
}

#[unsafe(no_mangle)]
pub fn icicle_get_exception_name(code: u32, callback: DataFunction, data: *mut c_void) {
    let name = format!("{:?}", icicle_cpu::ExceptionCode::from_u32(code));
    callback(data, name.as_ptr() as *const c_void, name.len());
}

#[unsafe(no_mangle)]
pub fn icicle_stop(ptr: *mut c_void) {
    if ptr.is_null() {
        return;
    }
    let stopped_on_owner = RUNNING_STOP.with(|slot| {
        if let Some((owner, stop)) = slot.borrow().as_ref() {
            if *owner == ptr as usize {
                *stop.borrow_mut() = true;
                return true;
            }
        }
        false
    });
    if stopped_on_owner {
        return;
    }
    // Clone while locked so destruction cannot free the flag before this store.
    let flag = stop_flags().lock().unwrap().get(&(ptr as usize)).cloned();
    if let Some(flag) = flag {
        flag.store(true, Ordering::Release);
    }
}

type RawFunction = extern "C" fn(*mut c_void);
type InstructionFunction = extern "C" fn(*mut c_void) -> u32;
type PtrFunction = extern "C" fn(*mut c_void, u64);
type BlockFunction = extern "C" fn(*mut c_void, u64, u64);
type DataFunction = extern "C" fn(*mut c_void, *const c_void, usize);
type MmioReadFunction = extern "C" fn(*mut c_void, u64, *mut c_void, usize);
type MmioWriteFunction = extern "C" fn(*mut c_void, u64, *const c_void, usize);
type ViolationFunction = extern "C" fn(*mut c_void, u64, u8, i32) -> i32;
type InterruptFunction = extern "C" fn(*mut c_void, i32);
type MemoryAccessFunction = MmioWriteFunction;
type WriteObservationFunction = extern "C" fn(*mut c_void, u64, *const c_void, usize, u64, i32);

#[unsafe(no_mangle)]
pub fn icicle_map_mmio(
    ptr: *mut c_void,
    address: u64,
    length: u64,
    read_cb: MmioReadFunction,
    read_data: *mut c_void,
    write_cb: MmioWriteFunction,
    write_data: *mut c_void,
) -> i32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);

        let read_wrapper = Box::new(move |addr: u64, data: &mut [u8]| {
            let raw_pointer: *mut u8 = data.as_mut_ptr();
            read_cb(read_data, addr, raw_pointer as *mut c_void, data.len());
        });

        let write_wrapper = Box::new(move |addr: u64, data: &[u8]| {
            let raw_pointer: *const u8 = data.as_ptr();
            write_cb(write_data, addr, raw_pointer as *const c_void, data.len());
        });

        let res = emulator.map_mmio(address, length, read_wrapper, write_wrapper);
        return to_cbool(res);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_map_memory(ptr: *mut c_void, address: u64, length: u64, permissions: u8) -> i32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let res = emulator.map_memory(address, length, permissions);
        return to_cbool(res);
    }
}

#[unsafe(no_mangle)]
pub unsafe fn icicle_map_host_memory(ptr: *mut c_void, address: u64, pointer: *mut u8, length: u64, permissions: u8) -> i32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        to_cbool(emulator.map_host_memory(address, pointer, length, permissions))
    }
}

#[unsafe(no_mangle)]
pub unsafe fn icicle_has_host_mappings(ptr: *mut c_void) -> i32 {
    unsafe {
        let emulator = &*(ptr as *mut IcicleEmulator);
        to_cbool(emulator.has_host_mappings())
    }
}

#[unsafe(no_mangle)]
pub unsafe fn icicle_flush_host_memory_cache(ptr: *mut c_void, pointer: *const c_void, length: usize) {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        emulator.flush_host_memory_cache(pointer as usize, length);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_map_shared_memory(ptr: *mut c_void, address: u64, source: u64, length: u64, permissions: u8) -> i32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        to_cbool(emulator.map_shared_memory(address, source, length, permissions))
    }
}

// SMP (multi-vCPU) shared guest RAM: the master VM maps fresh shared pages; each other vCPU VM maps
// the same Arc<PageData> backing so all N share one coherent, full-speed address space.
#[unsafe(no_mangle)]
pub fn icicle_map_smp_shared_fresh(ptr: *mut c_void, address: u64, length: u64, permissions: u8) -> i32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        to_cbool(emulator.map_smp_shared_fresh_range(address, length, permissions))
    }
}

#[unsafe(no_mangle)]
pub fn icicle_share_smp_pages(dst: *mut c_void, src: *mut c_void, address: u64, length: u64) -> i32 {
    unsafe {
        let dst_emulator = &mut *(dst as *mut IcicleEmulator);
        let src_emulator = &*(src as *mut IcicleEmulator);
        to_cbool(dst_emulator.share_smp_pages_from(src_emulator, address, length))
    }
}

// SMP async (step 6.5): capture the shared pages of a range from the SOURCE VM on its own thread, so a peer
// can alias them later without a cross-thread read of the source. The opaque handle owns the captured Arcs.
type SmpCapture = Vec<std::sync::Arc<icicle_cpu::mem::physical::PageData>>;

#[unsafe(no_mangle)]
pub fn icicle_smp_capture(ptr: *mut c_void, address: u64, length: u64) -> *mut c_void {
    unsafe {
        let emulator = &*(ptr as *mut IcicleEmulator);
        match emulator.capture_smp_range(address, length) {
            Some(pages) => Box::into_raw(Box::new(pages)) as *mut c_void,
            None => std::ptr::null_mut(),
        }
    }
}

#[unsafe(no_mangle)]
pub fn icicle_smp_map_captured(ptr: *mut c_void, captured: *mut c_void, address: u64) -> i32 {
    unsafe {
        if captured.is_null() {
            return 0;
        }
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let pages = &*(captured as *const SmpCapture);
        to_cbool(emulator.map_captured_smp_range(pages, address))
    }
}

#[unsafe(no_mangle)]
pub fn icicle_smp_release_capture(captured: *mut c_void) {
    if !captured.is_null() {
        unsafe {
            drop(Box::from_raw(captured as *mut SmpCapture));
        }
    }
}

#[unsafe(no_mangle)]
pub fn icicle_unmap_memory(ptr: *mut c_void, address: u64, length: u64) -> i32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let res = emulator.unmap_memory(address, length);
        return to_cbool(res);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_protect_memory(ptr: *mut c_void, address: u64, length: u64, permissions: u8) -> i32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let res = emulator.protect_memory(address, length, permissions);
        return to_cbool(res);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_write_memory(
    ptr: *mut c_void,
    address: u64,
    data: *const c_void,
    size: usize,
) -> i32 {
    if size == 0 {
        return 1;
    }

    if data.is_null() {
        return 0;
    }

    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let u8_slice = std::slice::from_raw_parts(data as *const u8, size);
        let res = emulator.write_memory(address, u8_slice);
        return to_cbool(res);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_save_registers(ptr: *mut c_void, accessor: DataFunction, accessor_data: *mut c_void) {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let registers = emulator.save_registers();
        accessor(
            accessor_data,
            registers.as_ptr() as *const c_void,
            registers.len(),
        );
    }
}

#[unsafe(no_mangle)]
pub fn icicle_restore_registers(ptr: *mut c_void, data: *const c_void, size: usize) {
    if size == 0 || data.is_null() {
        return;
    }

    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let u8_slice = std::slice::from_raw_parts(data as *const u8, size);
        emulator.restore_registers(u8_slice);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_reset_volatile_state(ptr: *mut c_void) {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        emulator.reset_volatile_state();
    }
}

#[unsafe(no_mangle)]
pub fn icicle_create_snapshot(ptr: *mut c_void) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.create_snapshot();
    }
}

#[unsafe(no_mangle)]
pub fn icicle_restore_snapshot(ptr: *mut c_void, id: u32) {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        emulator.restore_snapshot(id);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_read_memory(ptr: *mut c_void, address: u64, data: *mut c_void, size: usize) -> i32 {
    if size == 0 {
        return 1;
    }

    if data.is_null() {
        return 0;
    }

    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let u8_slice = std::slice::from_raw_parts_mut(data as *mut u8, size);
        let res = emulator.read_memory(address, u8_slice);
        return to_cbool(res);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_interrupt_hook(
    ptr: *mut c_void,
    callback: InterruptFunction,
    data: *mut c_void,
) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.add_interrupt_hook(Box::new(move |code: i32| callback(data, code)));
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_violation_hook(
    ptr: *mut c_void,
    callback: ViolationFunction,
    data: *mut c_void,
) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.add_violation_hook(Box::new(
            move |address: u64, permission: u8, unmapped: bool| {
                let result = callback(data, address, permission, to_cbool(unmapped));
                if result == 0 {
                    return false;
                }

                return true;
            },
        ));
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_read_hook(
    ptr: *mut c_void,
    start: u64,
    end: u64,
    callback: MemoryAccessFunction,
    user: *mut c_void,
) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.add_read_hook(
            start,
            end,
            Box::new(move |address: u64, data: &[u8]| {
                callback(user, address, data.as_ptr() as *const c_void, data.len());
            }),
        );
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_write_hook(
    ptr: *mut c_void,
    start: u64,
    end: u64,
    callback: MemoryAccessFunction,
    user: *mut c_void,
) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.add_write_hook(
            start,
            end,
            Box::new(move |address: u64, data: &[u8]| {
                callback(user, address, data.as_ptr() as *const c_void, data.len());
            }),
        );
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn icicle_add_write_observation_hook(
    ptr: *mut c_void,
    start: u64,
    end: u64,
    callback: WriteObservationFunction,
    user: *mut c_void,
) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        emulator.add_write_observation_hook(
            start,
            end,
            Box::new(move |address, data, error, host_write| {
                callback(
                    user,
                    address,
                    data.as_ptr() as *const c_void,
                    data.len(),
                    error,
                    i32::from(host_write),
                );
            }),
        )
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_syscall_hook(ptr: *mut c_void, callback: RawFunction, data: *mut c_void) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.add_syscall_hook(Box::new(move || callback(data)));
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_timestamp_hook(
    ptr: *mut c_void,
    serializing: i32,
    callback: InstructionFunction,
    data: *mut c_void,
) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        emulator.add_timestamp_hook(serializing != 0, Box::new(move || callback(data)))
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_block_hook(ptr: *mut c_void, callback: BlockFunction, data: *mut c_void) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.add_block_hook(Box::new(move |address: u64, instructions: u64| {
            callback(data, address, instructions)
        }));
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_generic_execution_hook(
    ptr: *mut c_void,
    callback: PtrFunction,
    data: *mut c_void,
) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.add_generic_execution_hook(Box::new(move |ptr: u64| callback(data, ptr)));
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_execution_hook(
    ptr: *mut c_void,
    address: u64,
    callback: PtrFunction,
    data: *mut c_void,
) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.add_execution_hook(address, Box::new(move |ptr: u64| callback(data, ptr)));
    }
}

#[unsafe(no_mangle)]
pub fn icicle_add_ranged_execution_hook(
    ptr: *mut c_void,
    address: u64,
    size: u64,
    callback: PtrFunction,
    data: *mut c_void,
) -> u32 {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        return emulator.add_ranged_execution_hook(
            address,
            size,
            Box::new(move |ptr: u64| callback(data, ptr)),
        );
    }
}

#[unsafe(no_mangle)]
pub fn icicle_remove_hook(ptr: *mut c_void, id: u32) {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        emulator.remove_hook(id);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_run_on_next_instruction(ptr: *mut c_void, callback: RawFunction, data: *mut c_void) {
    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        emulator.run_on_next_instruction(Box::new(move || callback(data)));
    }
}

#[unsafe(no_mangle)]
pub fn icicle_read_register(
    ptr: *mut c_void,
    reg: X86Register,
    data: *mut c_void,
    size: usize,
) -> usize {
    if size == 0 {
        return 1;
    }

    if data.is_null() {
        return 0;
    }

    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let u8_slice = std::slice::from_raw_parts_mut(data as *mut u8, size);
        return emulator.read_register(reg, u8_slice);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_write_register(
    ptr: *mut c_void,
    reg: X86Register,
    data: *const c_void,
    size: usize,
) -> usize {
    if size == 0 {
        return 1;
    }

    if data.is_null() {
        return 0;
    }

    unsafe {
        let emulator = &mut *(ptr as *mut IcicleEmulator);
        let u8_slice = std::slice::from_raw_parts(data as *const u8, size);
        return emulator.write_register(reg, u8_slice);
    }
}

#[unsafe(no_mangle)]
pub fn icicle_destroy_emulator(ptr: *mut c_void) {
    if ptr.is_null() {
        return;
    }

    stop_flags().lock().unwrap().remove(&(ptr as usize));
    unsafe {
        let _ = Box::from_raw(ptr as *mut IcicleEmulator);
    }
}

#[cfg(test)]
mod stop_tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;

    #[test]
    fn remote_stop_updates_only_registered_atomic() {
        let ptr = icicle_create_emulator(0);
        assert!(!ptr.is_null());
        let flag = stop_flags().lock().unwrap().get(&(ptr as usize)).cloned().unwrap();
        assert!(!flag.load(Ordering::Acquire));

        let address = ptr as usize;
        std::thread::spawn(move || icicle_stop(address as *mut c_void)).join().unwrap();
        assert!(flag.load(Ordering::Acquire));

        icicle_destroy_emulator(ptr);
        assert!(!stop_flags().lock().unwrap().contains_key(&address));
    }

    #[test]
    fn remote_stop_before_start_retires_no_instructions_and_is_consumed() {
        let ptr = icicle_create_emulator(0);
        assert!(!ptr.is_null());
        let emulator = unsafe { &mut *(ptr as *mut IcicleEmulator) };
        assert!(emulator.map_memory(0x10000, 4096, 7));
        assert!(emulator.write_memory(0x10000, &[0xeb, 0xfe]));
        assert_eq!(emulator.write_register(X86Register::Rip, &0x10000u64.to_le_bytes()), 8);
        icicle_stop(ptr);
        icicle_start(ptr, 100);
        assert_eq!(icicle_get_icount(ptr), 0);
        let mut info = IcicleStopInfo { kind: 0, code: 0, value: 0 };
        assert_eq!(icicle_get_stop_info(ptr, &mut info), 1);
        assert_eq!(info.kind, 1); // instruction-limit stop kind in the FFI contract
        assert!(!stop_flags().lock().unwrap().get(&(ptr as usize)).unwrap().load(Ordering::Acquire));

        icicle_start(ptr, 2);
        assert_eq!(icicle_get_icount(ptr), 2);
        icicle_destroy_emulator(ptr);
    }

    extern "C" fn signal_first_instruction(data: *mut c_void, _: u64) {
        let seen = unsafe { &*(data as *const AtomicBool) };
        seen.store(true, Ordering::Release);
    }

    #[test]
    fn remote_stop_exits_a_running_vm() {
        let ptr = icicle_create_emulator(0);
        assert!(!ptr.is_null());
        let emulator = unsafe { &mut *(ptr as *mut IcicleEmulator) };
        assert!(emulator.map_memory(0x10000, 4096, 7));
        assert!(emulator.write_memory(0x10000, &[0xeb, 0xfe]));
        assert_eq!(emulator.write_register(X86Register::Rip, &0x10000u64.to_le_bytes()), 8);

        let seen = Arc::new(AtomicBool::new(false));
        let id = icicle_add_execution_hook(ptr, 0x10000, signal_first_instruction, Arc::as_ptr(&seen) as *mut c_void);
        assert_ne!(id, 0);
        let address = ptr as usize;
        let worker = std::thread::spawn(move || {
            icicle_start(address as *mut c_void, 100_000_000);
            let mut info = IcicleStopInfo { kind: 0, code: 0, value: 0 };
            assert_eq!(icicle_get_stop_info(address as *mut c_void, &mut info), 1);
            (info, icicle_get_icount(address as *mut c_void))
        });
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        while !seen.load(Ordering::Acquire) && std::time::Instant::now() < deadline {
            std::thread::yield_now();
        }
        assert!(seen.load(Ordering::Acquire), "guest did not execute the entry block");
        icicle_stop(ptr);
        let (info, count) = worker.join().unwrap();
        assert_eq!(info.kind, 1); // instruction-limit stop kind in the FFI contract
        assert!(count < 50_000_000, "remote stop did not interrupt the run: {count}");
        icicle_destroy_emulator(ptr);
    }

    struct OwnerInterruptStop {
        emulator: usize,
        hits: AtomicUsize,
    }

    extern "C" fn stop_on_guest_interrupt(data: *mut c_void, code: i32) {
        let state = unsafe { &*(data as *const OwnerInterruptStop) };
        assert_eq!(code, 41); // int 29h fast-fail interrupt
        state.hits.fetch_add(1, Ordering::Relaxed);
        icicle_stop(state.emulator as *mut c_void);
    }

    #[test]
    fn owner_stop_inside_interrupt_hook_ends_before_reexecuting_int29() {
        let ptr = icicle_create_emulator(0);
        assert!(!ptr.is_null());
        let emulator = unsafe { &mut *(ptr as *mut IcicleEmulator) };
        assert!(emulator.map_memory(0x10000, 4096, 7));
        // mov ecx, 7; int 29h; jmp $ — the fast-fail path may not retire int29.
        assert!(emulator.write_memory(0x10000, &[0xb9, 7, 0, 0, 0, 0xcd, 0x29, 0xeb, 0xfe]));
        assert_eq!(emulator.write_register(X86Register::Rip, &0x10000u64.to_le_bytes()), 8);

        let mut state = Box::new(OwnerInterruptStop {
            emulator: ptr as usize,
            hits: AtomicUsize::new(0),
        });
        assert_ne!(icicle_add_interrupt_hook(ptr, stop_on_guest_interrupt,
            (&mut *state as *mut OwnerInterruptStop).cast()), 0);
        icicle_start(ptr, 100);

        let mut info = IcicleStopInfo { kind: 0, code: 0, value: 0 };
        assert_eq!(icicle_get_stop_info(ptr, &mut info), 1);
        assert_eq!(info.kind, 1); // owner-requested stop maps to instruction limit
        assert_eq!(state.hits.load(Ordering::Relaxed), 1);
        assert!(icicle_get_icount(ptr) < 100);
        icicle_destroy_emulator(ptr);
    }

    #[test]
    fn owner_stop_marks_execution_hook_and_atomic() {
        let ptr = icicle_create_emulator(0);
        assert!(!ptr.is_null());
        let owner_stop = unsafe { (&*(ptr as *mut IcicleEmulator)).owner_stop_cell() };
        let flag = stop_flags().lock().unwrap().get(&(ptr as usize)).cloned().unwrap();
        let owner = RunningStopGuard::new(ptr as usize, Rc::clone(&owner_stop));

        icicle_stop(ptr);
        assert!(*owner_stop.borrow());
        assert!(!flag.load(Ordering::Acquire));

        drop(owner);
        icicle_destroy_emulator(ptr);
    }
}

#[cfg(test)]
mod architecture_tests {
    #[test]
    fn build_both_x86_languages() {
        for target in ["i686-none", "x86_64-none"] {
            icicle_vm::build(&icicle_cpu::Config::from_target_triple(target)).unwrap();
        }
    }
}
