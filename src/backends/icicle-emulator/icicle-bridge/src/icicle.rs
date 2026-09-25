use icicle_cpu::ExceptionCode;
use icicle_cpu::ValueSource;
use std::cell::Cell;
use std::collections::{BTreeMap, HashSet};
use std::time::Instant;
use std::{cell::RefCell, collections::HashMap, rc::Rc};

use crate::registers;

// SMP 6.7 LOCK-op atomicity: the x86 SLEIGH spec emits LOCK()/UNLOCK() pcode userops around
// every lockable RMW; icicle executes the RMW as plain load/compare/store pcode, so a peer
// vCPU's op on the same shared variable interleaves and tears the atomic (the probe's terminal
// 0x43 garbage record traces to torn LOCK cmpxchg in ntdll thread startup). The hooks installed
// below acquire/release a process-global RMW spinlock: LOCK-vs-LOCK mutual exclusion across all
// vCPU VMs (the realistic corruption class - synchronization variables are always LOCK-accessed).
mod smp_rmw_lock {
    use std::sync::atomic::{AtomicBool, Ordering};

    static RMW_LOCK: AtomicBool = AtomicBool::new(false);
    thread_local! {
        static HELD: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
    }

    pub fn acquire() {
        if HELD.with(|h| h.get()) {
            return; // reentrant (exception-path safety release may precede a nested acquire)
        }
        while RMW_LOCK.compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed).is_err() {
            std::hint::spin_loop();
        }
        HELD.with(|h| h.set(true));
    }

    pub fn release() {
        if HELD.with(|h| h.replace(false)) {
            RMW_LOCK.store(false, Ordering::Release);
        }
    }
}

fn create_x64_vm() -> icicle_vm::Vm {
    let mut cpu_config = icicle_vm::cpu::Config::from_target_triple("x86_64-none");
    cpu_config.enable_jit = std::env::var("SOGEN_ICICLE_JIT").as_deref() == Ok("1");
    // SOGEN_ICICLE_JIT_MEM=0 disables the JIT's compiled memory fast path. Hooked blocks never
    // use it (hook ops force slow paths); lean blocks do — and it runs through the vendored
    // icicle-mem layer (our smp_shared rework). The lean-miscompile discriminator.
    cpu_config.enable_jit_mem = std::env::var("SOGEN_ICICLE_JIT_MEM").map(|v| v != "0").unwrap_or(true);
    cpu_config.enable_shadow_stack = false;
    cpu_config.enable_recompilation = std::env::var("SOGEN_ICICLE_RECOMP").map(|v| v != "0").unwrap_or(true);
    cpu_config.track_uninitialized = false;
    // SOGEN_ICICLE_OPT_INSTR=0 disables the pcode instruction optimizer — the lean-mode
    // (hookless) JIT miscompile discriminator: hooks fragment optimized sequences, so the
    // optimizer's lean path was never exercised before lean mode existed.
    cpu_config.optimize_instructions = std::env::var("SOGEN_ICICLE_OPT_INSTR").map(|v| v != "0").unwrap_or(true);
    cpu_config.optimize_block = false;

    let mut vm = icicle_vm::build(&cpu_config).unwrap();
    vm.lifter.settings.max_instructions_per_block =
        if std::env::var("SOGEN_ICICLE_MAX_BLOCK_INSTRUCTIONS").as_deref() == Ok("32") {
            32
        } else {
            128
        };
    // Upstream builder only copies enable_jit from Config. Propagate this bridge flag to the
    // field actually checked by Vm::run() before its periodic recompilation.
    vm.enable_recompilation = cpu_config.enable_recompilation;

    // SMP 6.7: register LOCK()/UNLOCK() hooks (acquire/release the RMW spinlock) and op
    // injectors that rewrite those pcode userops into the hooks. If the locked instruction
    // faults between LOCK and UNLOCK, the exception path releases (see handle_exception).
    {
        use icicle_cpu::lifter::BlockState;
        use icicle_cpu::{HookHandler, InstHook};
        use icicle_cpu::{Arch, Cpu};

        struct LockAcquire;
        impl HookHandler for LockAcquire {
            fn call(_: &mut Self, _: &mut Cpu, _: u64) {
                smp_rmw_lock::acquire();
            }
        }
        struct LockRelease;
        impl HookHandler for LockRelease {
            fn call(_: &mut Self, _: &mut Cpu, _: u64) {
                smp_rmw_lock::release();
            }
        }

        let acquire_id = vm.cpu.trace.add_hook(InstHook::new(LockAcquire));
        let release_id = vm.cpu.trace.add_hook(InstHook::new(LockRelease));
        let _ = vm.add_op_injector("LOCK", move |_: &Arch, _: pcode::PcodeOpId, _: pcode::Inputs, _: pcode::VarNode, state: &mut BlockState| {
            state.pcode.push((pcode::Op::Hook(acquire_id), pcode::Inputs::none()));
            false
        });
        let _ = vm.add_op_injector("UNLOCK", move |_: &Arch, _: pcode::PcodeOpId, _: pcode::Inputs, _: pcode::VarNode, state: &mut BlockState| {
            state.pcode.push((pcode::Op::Hook(release_id), pcode::Inputs::none()));
            false
        });
    }
    crate::aes::register(&mut vm.cpu);
    crate::packed_sad::register(&mut vm.cpu);
    crate::reciprocal_sqrt::register(&mut vm.cpu);
    crate::xstate::register(&mut vm.cpu);
    crate::aligned_move::register(&mut vm.cpu);
    crate::simd_minmax::register(&mut vm.cpu);
    crate::packed_mul_high::register(&mut vm.cpu);
    vm
}

const CACHE_INVALIDATED: u64 = 0x10000;

const FOREIGN_READ: u8 = 1 << 0;
const FOREIGN_WRITE: u8 = 1 << 1;
const FOREIGN_EXEC: u8 = 1 << 2;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct IcicleStopInfo {
    pub kind: u32,
    pub code: u32,
    pub value: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct IcicleJitProfile {
    pub compile_calls: u64,
    pub compile_nanos: u64,
    pub reset_calls: u64,
    pub recompile_calls: u64,
    pub recompile_nanos: u64,
    pub recompile_compile_calls: u64,
    pub recompile_compile_nanos: u64,
    pub reset_generation: u64,
    pub reset_cause_flags: u64,
    pub reset_manual_origin_flags: u64,
    pub flush_code_nanos: u64,
    pub jit_reset_nanos: u64,
    pub generation_compile_calls: u64,
    pub generation_compile_nanos: u64,
    pub origin_first_address_compiles: u64,
    pub origin_repeat_after_reset_compiles: u64,
    pub origin_repeat_in_generation_compiles: u64,
    pub origin_periodic_recompile_compiles: u64,
    pub origin_unclassified_compiles: u64,
    pub origin_generation_number: u64,
}

impl IcicleStopInfo {
    const NONE: u32 = 0;
    const INSTRUCTION_LIMIT: u32 = 1;
    const UNHANDLED_EXCEPTION: u32 = 2;
    const OTHER: u32 = 3;

    fn none() -> Self {
        Self {
            kind: Self::NONE,
            code: 0,
            value: 0,
        }
    }

    fn instruction_limit() -> Self {
        Self {
            kind: Self::INSTRUCTION_LIMIT,
            code: 0,
            value: 0,
        }
    }

    fn unhandled_exception(code: ExceptionCode, value: u64) -> Self {
        Self {
            kind: Self::UNHANDLED_EXCEPTION,
            code: code as u32,
            value,
        }
    }

    fn other(code: u32, value: u64) -> Self {
        Self {
            kind: Self::OTHER,
            code,
            value,
        }
    }
}

fn map_permissions(foreign_permissions: u8) -> u8 {
    let mut permissions: u8 = 0;

    if (foreign_permissions & FOREIGN_READ) != 0 {
        permissions |= icicle_vm::cpu::mem::perm::READ;
    }

    if (foreign_permissions & FOREIGN_WRITE) != 0 {
        permissions |= icicle_vm::cpu::mem::perm::WRITE;
    }

    if (foreign_permissions & FOREIGN_EXEC) != 0 {
        permissions |= icicle_vm::cpu::mem::perm::EXEC;
    }

    return permissions;
}

#[repr(u8)]
#[allow(dead_code)]
#[derive(PartialEq)]
enum HookType {
    Syscall = 1,
    Read,
    Write,
    ExecuteGeneric,
    ExecuteSpecific,
    ExecuteRange,
    Violation,
    Interrupt,
    Block,
    Timestamp,
    TimestampSerializing,
    Unknown,
}

fn u8_to_hook_type_unsafe(value: u8) -> HookType {
    unsafe { std::mem::transmute(value) }
}

fn split_hook_id(id: u32) -> (u32, HookType) {
    let hook_id = id & 0xFFFFFF;
    let hook_type = u8_to_hook_type_unsafe((id >> 24) as u8);

    return (hook_id, hook_type);
}

fn qualify_hook_id(hook_id: u32, hook_type: HookType) -> u32 {
    let hook_type: u32 = (hook_type as u8).into();
    let hook_type_mask: u32 = hook_type << 24;
    return (hook_id | hook_type_mask).into();
}

fn is_within_start_and_length(value: u64, start: u64, length: u64) -> bool {
    return value >= start && value < start.wrapping_add(length);
}

pub struct HookContainer<Func: ?Sized> {
    hook_id: u32,
    is_iterating: bool,
    hooks: Vec<(u32, Box<Func>)>,
    hooks_to_add: HashMap<u32, Box<Func>>,
    hooks_to_remove: HashSet<u32>,
}

impl<Func: ?Sized> HookContainer<Func> {
    pub fn new() -> Self {
        Self {
            hook_id: 0,
            is_iterating: false,
            hooks: Vec::new(),
            hooks_to_add: HashMap::new(),
            hooks_to_remove: HashSet::new(),
        }
    }

    pub fn add_hook(&mut self, callback: Box<Func>) -> u32 {
        self.hook_id += 1;
        let id = self.hook_id;

        if self.is_iterating {
            self.hooks_to_add.insert(id, callback);
        } else {
            self.hooks.push((id, callback));
        }

        return id;
    }

    pub fn for_each_hook<F>(&mut self, mut callback: F)
    where
        F: FnMut(&Func),
    {
        match self.hooks.len() {
            0 => (),
            1 => {
                let was_iterating = self.do_pre_access_work();
                callback(self.hooks[0].1.as_ref());
                self.do_post_access_work(was_iterating);
            }
            _ => self.for_each_multiple_hook(callback),
        }
    }

    #[inline(never)]
    fn for_each_multiple_hook<F>(&mut self, mut callback: F)
    where
        F: FnMut(&Func),
    {
        let was_iterating = self.do_pre_access_work();
        for (_, func) in &self.hooks {
            callback(func.as_ref());
        }
        self.do_post_access_work(was_iterating);
    }

    pub fn access_hook<F>(&mut self, id: u32, mut callback: F)
    where
        F: FnMut(&Func),
    {
        let was_iterating = self.do_pre_access_work();

        if let Some((_, hook)) = self.hooks.iter().find(|(hook_id, _)| *hook_id == id) {
            callback(hook.as_ref());
        }

        self.do_post_access_work(was_iterating);
    }

    pub fn is_empty(&self) -> bool {
        return self.hooks.is_empty();
    }

    pub fn remove_hook(&mut self, id: u32) {
        if self.is_iterating {
            self.hooks_to_remove.insert(id);
        } else {
            self.hooks.retain(|(hook_id, _)| *hook_id != id);
        }
    }

    fn do_pre_access_work(&mut self) -> bool {
        let was_iterating = self.is_iterating;
        self.is_iterating = true;
        return was_iterating;
    }

    #[inline]
    fn do_post_access_work(&mut self, was_iterating: bool) {
        self.is_iterating = was_iterating;
        if !self.is_iterating && (!self.hooks_to_remove.is_empty() || !self.hooks_to_add.is_empty())
        {
            self.apply_pending_changes();
        }
    }

    #[cold]
    #[inline(never)]
    fn apply_pending_changes(&mut self) {
        if !self.hooks_to_remove.is_empty() {
            let to_remove = std::mem::take(&mut self.hooks_to_remove);
            self.hooks.retain(|(id, _)| !to_remove.contains(id));
        }
        if !self.hooks_to_add.is_empty() {
            let to_add = std::mem::take(&mut self.hooks_to_add);
            self.hooks.extend(to_add);
        }
    }
}

struct InstructionHookInjector {
    inst_hook: pcode::HookId,
    block_hook: pcode::HookId,
    page_hook: pcode::HookId,
    emit_block_hook: bool,
    check_page_transitions: bool,
    block_only: bool,
}

fn count_instructions(block: &icicle_cpu::lifter::Block) -> u64 {
    return block.pcode.instructions.iter().fold(0u64, |count, &stmt| {
        if let pcode::Op::InstructionMarker = stmt.op {
            return count + 1;
        }

        return count;
    });
}

impl icicle_vm::CodeInjector for InstructionHookInjector {
    fn inject(
        &mut self,
        cpu: &mut icicle_vm::cpu::Cpu,
        group: &icicle_vm::cpu::BlockGroup,
        code: &mut icicle_vm::BlockTable,
    ) {
        for id in group.range() {
            let block = &mut code.blocks[id];

            let mut tmp_block = pcode::Block::new();
            tmp_block.next_tmp = block.pcode.next_tmp;

            let mut is_first_inst = true;
            let mut previous_page = None;
            let inst_count = count_instructions(&block);

            let mut replace_instruction = false;
            for stmt in block.pcode.instructions.drain(..) {
                if matches!(stmt.op, pcode::Op::InstructionMarker) {
                    replace_instruction = false;
                } else if replace_instruction {
                    continue;
                }
                tmp_block.push(stmt);
                if let pcode::Op::InstructionMarker = stmt.op {
                    let page = cpu.mem.page_aligned(stmt.inputs.first().as_u64());
                    if is_first_inst {
                        is_first_inst = false;
                        if self.emit_block_hook {
                            tmp_block.push((pcode::Op::Arg(0), pcode::Inputs::one(inst_count)));
                            tmp_block.push(pcode::Op::Hook(self.block_hook));
                        }
                    } else if self.check_page_transitions && previous_page != Some(page) {
                        tmp_block.push(pcode::Op::Hook(self.page_hook));
                    }
                    previous_page = Some(page);

                    if !self.block_only {
                        tmp_block.push(pcode::Op::Hook(self.inst_hook));
                    }
                    let length = stmt.inputs.second().as_u64();
                    let mut bytes = [0u8; 15];
                    if length <= 15
                        && cpu
                            .mem
                            .read_bytes(
                                stmt.inputs.first().as_u64(),
                                &mut bytes[..length as usize],
                                0,
                            )
                            .is_ok()
                    {
                        let instruction = &bytes[..length as usize];
                        let prefix_length = instruction
                            .iter()
                            .take_while(|&&byte| {
                                matches!(
                                    byte,
                                    0x26 | 0x2e
                                        | 0x36
                                        | 0x3e
                                        | 0x64
                                        | 0x65
                                        | 0x66
                                        | 0x67
                                        | 0xf2
                                        | 0xf3
                                        | 0x40..=0x4f
                                )
                            })
                            .count();
                        let kind = match &instruction[prefix_length..] {
                            [0x0f, 0x31] => 1u64,
                            [0x0f, 0x01, 0xf9] => 2u64,
                            _ => 0u64,
                        };
                        if kind != 0 {
                            tmp_block.push((
                                pcode::Op::Exception,
                                (ExceptionCode::Environment as u64, (kind << 8) | length),
                            ));
                            replace_instruction = true;
                        }
                    }
                    code.modified.insert(id);
                }
            }

            std::mem::swap(&mut tmp_block.instructions, &mut block.pcode.instructions);
        }
    }
}

#[inline]
fn smp_dbg(msg: &str) {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    if *ENABLED.get_or_init(|| std::env::var("SOGEN_SMP_DEBUG").as_deref() == Ok("1")) {
        eprintln!("[SMPDBG-R] {msg}");
    }
}

fn smp_lean_epoch_hook_enabled() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ENABLED.get_or_init(|| std::env::var("SOGEN_SMP_LEAN_EPOCH_HOOK").as_deref() == Ok("1"))
}

fn smp_prelift_epoch_enabled() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ENABLED.get_or_init(|| std::env::var("SOGEN_SMP_PRELIFT_EPOCH").as_deref() == Ok("1"))
}

fn range_invalidation_enabled() -> bool {
    std::env::var("SOGEN_ICICLE_RANGE_INVALIDATE").as_deref() == Ok("1")
}

/// SMP 6.6a epoch mode: 1 = smp_shared pages only (DEFAULT - production), 2 = all pages
/// (A/B experiment), 0 = off. The earlier "recovery fault" was a bad TEST writing the VALUE 0x2222
/// over the B8 opcode (re-lifted garbage read [0]); the mechanism itself is proven on private
/// (epoch_recovery_private_page_ab) and shared (GuestSelfModifyingCodeSeenByOtherVcpu) pages.
fn smp_epoch_mode() -> u8 {
    use std::sync::OnceLock;
    static MODE: OnceLock<u8> = OnceLock::new();
    *MODE.get_or_init(|| {
        std::env::var("SOGEN_SMP_EPOCH").map(|v| v.parse().unwrap_or(1)).unwrap_or(1).min(2)
    })
}

const INVALIDATION_EPOCH: u8 = 1;
const INVALIDATION_WAKE: u8 = 2;
const INVALIDATION_MANUAL: u8 = 4;
const MANUAL_PEER_PROTECTION: u16 = 1 << 0;
const MANUAL_PUBLIC_INVALIDATE: u16 = 1 << 1;
const MANUAL_SELF_MODIFYING: u16 = 1 << 2;
const MANUAL_HOST_CACHE: u16 = 1 << 3;
const MANUAL_UNMAP: u16 = 1 << 4;
const MANUAL_PROTECT: u16 = 1 << 5;
const MANUAL_HOST_WRITE: u16 = 1 << 6;

#[derive(Default)]
pub(crate) struct InvalidationProfile {
    pub epoch_mismatches: std::sync::atomic::AtomicU64,
    pub jit_resets: std::sync::atomic::AtomicU64,
    pub epoch_resets: std::sync::atomic::AtomicU64,
    pub wake_resets: std::sync::atomic::AtomicU64,
    pub manual_resets: std::sync::atomic::AtomicU64,
    pub mixed_resets: std::sync::atomic::AtomicU64,
    pub unknown_resets: std::sync::atomic::AtomicU64,
    pub manual_origin_resets: std::sync::atomic::AtomicU64,
    pub manual_peer_protection: std::sync::atomic::AtomicU64,
    pub manual_public_invalidate: std::sync::atomic::AtomicU64,
    pub manual_self_modifying: std::sync::atomic::AtomicU64,
    pub manual_host_cache: std::sync::atomic::AtomicU64,
    pub manual_unmap: std::sync::atomic::AtomicU64,
    pub manual_protect: std::sync::atomic::AtomicU64,
    pub manual_host_write: std::sync::atomic::AtomicU64,
    pub manual_multiple_origins: std::sync::atomic::AtomicU64,
    pub manual_unknown_origin: std::sync::atomic::AtomicU64,
}

impl InvalidationProfile {
    fn record_reset(&self, causes: u8, manual_origins: u16) {
        use std::sync::atomic::Ordering::Relaxed;
        self.jit_resets.fetch_add(1, Relaxed);
        let bucket = match causes {
            INVALIDATION_EPOCH => &self.epoch_resets,
            INVALIDATION_WAKE => &self.wake_resets,
            INVALIDATION_MANUAL => &self.manual_resets,
            0 => &self.unknown_resets,
            _ => &self.mixed_resets,
        };
        bucket.fetch_add(1, Relaxed);
        if causes & INVALIDATION_MANUAL != 0 {
            self.manual_origin_resets.fetch_add(1, Relaxed);
            let origin_bucket = match manual_origins {
                MANUAL_PEER_PROTECTION => &self.manual_peer_protection,
                MANUAL_PUBLIC_INVALIDATE => &self.manual_public_invalidate,
                MANUAL_SELF_MODIFYING => &self.manual_self_modifying,
                MANUAL_HOST_CACHE => &self.manual_host_cache,
                MANUAL_UNMAP => &self.manual_unmap,
                MANUAL_PROTECT => &self.manual_protect,
                MANUAL_HOST_WRITE => &self.manual_host_write,
                0 => &self.manual_unknown_origin,
                _ => &self.manual_multiple_origins,
            };
            origin_bucket.fetch_add(1, Relaxed);
        }
    }
}

struct ExecutionHooks {
    stop: Rc<RefCell<bool>>,
    /// Guest page -> this VM's mapping generation and last observed code epoch.
    shared_code_epochs: std::collections::HashMap<u64, (u64, u64)>,
    invalidate_code: Rc<Cell<bool>>,
    invalidation_causes: Option<Rc<Cell<u8>>>,
    invalidation_profile: Option<std::sync::Arc<InvalidationProfile>>,
    generic_hooks: HookContainer<dyn Fn(u64)>,
    ranged_hooks: HookContainer<dyn Fn(u64)>,
    specific_hooks: HookContainer<dyn Fn(u64)>,
    block_hooks: HookContainer<dyn Fn(u64, u64)>,
    address_mapping: BTreeMap<u64, Vec<u32>>,
    address_filter: [u64; 4],
    // Opt-in lean diagnostic: reject non-candidate exact addresses without borrowing the table.
    lean_exact_filter: Option<Rc<Cell<[u64; 4]>>>,
    one_time_callbacks: Vec<Box<dyn Fn()>>,
}

impl ExecutionHooks {
    pub fn new(stop_value: Rc<RefCell<bool>>, invalidate_code: Rc<Cell<bool>>) -> Self {
        Self {
            stop: stop_value,
            shared_code_epochs: std::collections::HashMap::new(),
            invalidate_code,
            invalidation_causes: None,
            invalidation_profile: None,
            generic_hooks: HookContainer::new(),
            ranged_hooks: HookContainer::new(),
            specific_hooks: HookContainer::new(),
            block_hooks: HookContainer::new(),
            address_mapping: BTreeMap::new(),
            address_filter: [0; 4],
            lean_exact_filter: None,
            one_time_callbacks: Vec::new(),
        }
    }

    fn address_filter_bit(address: u64) -> (usize, u64) {
        let hash = (address ^ (address >> 32)).wrapping_mul(0x9e3779b97f4a7c15);
        let bit = (hash >> 56) as usize;
        (bit / 64, 1 << (bit % 64))
    }

    #[cold]
    #[inline(never)]
    fn run_scheduled_callbacks(&mut self) {
        let callbacks = std::mem::take(&mut self.one_time_callbacks);
        for cb in callbacks {
            cb.as_ref()();
        }
    }

    fn run_hooks(&mut self, address: u64) {
        if !self.one_time_callbacks.is_empty() {
            self.run_scheduled_callbacks();
        }

        self.generic_hooks.for_each_hook(|func| {
            func(address);
        });

        if !self.ranged_hooks.is_empty() {
            self.run_ranged_hooks(address);
        }

        let (word, bit) = Self::address_filter_bit(address);
        if self.address_filter[word] & bit != 0 {
            self.run_specific_hooks(address);
        }
    }

    #[inline(never)]
    fn run_ranged_hooks(&mut self, address: u64) {
        self.ranged_hooks.for_each_hook(|func| {
            func(address);
        });
    }

    #[inline(never)]
    fn run_specific_hooks(&mut self, address: u64) {
        let mapping = self.address_mapping.get(&address);
        if mapping.is_none() {
            return;
        }

        for id in mapping.unwrap() {
            self.specific_hooks.access_hook(*id, |func| {
                func(address);
            });
        }
    }

    pub fn on_block(&mut self, address: u64, instructions: u64) {
        self.block_hooks.for_each_hook(|func| {
            func(address, instructions);
        });
    }

    fn check_code_epoch(&mut self, cpu: &mut icicle_cpu::Cpu, address: u64) {
        // The map-time baseline is experimental because loader writes before first lift can
        // otherwise cause one full code-cache flush per DLL page.
        {
            let mem = &cpu.mem;
            let page_start = mem.page_aligned(address);
            if let Some(index) = mem.get_physical_index(page_start) {
                let page = mem.get_physical(index);
                if (page.smp_shared || smp_epoch_mode() == 2) && smp_epoch_mode() != 0 {
                    let current = page.data().code_epoch();
                    let generation = page.smp_mapping_generation;
                    let baseline = if page.smp_shared && smp_prelift_epoch_enabled() {
                        page.smp_mapping_epoch
                    } else {
                        current
                    };
                    let seen = self
                        .shared_code_epochs
                        .entry(page_start)
                        .or_insert((generation, baseline));
                    if seen.0 != generation || seen.1 != current {
                        smp_dbg(&format!(
                            "epoch mismatch page={page_start:#x} seen={} current={current} pc={address:#x}",
                            seen.1
                        ));
                        *seen = (generation, current);
                        if let Some(causes) = &self.invalidation_causes {
                            causes.set(causes.get() | INVALIDATION_EPOCH);
                        }
                        if let Some(profile) = &self.invalidation_profile {
                            profile.epoch_mismatches.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                        }
                        self.invalidate_code.set(true);
                        // Full hooks raise after callbacks because an earlier raise broke refetch
                        // after flushing the code cache.
                    }
                }
            }
        }
    }

    pub fn execute(&mut self, cpu: &mut icicle_cpu::Cpu, address: u64) {
        if self.invalidate_code.get() {
            cpu.exception =
                icicle_cpu::Exception::new(ExceptionCode::Environment, CACHE_INVALIDATED);
            return;
        }
        self.check_code_epoch(cpu, address);
        self.run_hooks(address);

        if *self.stop.borrow() {
            cpu.exception.code = ExceptionCode::InstructionLimit as u32;
            cpu.exception.value = address;
        } else if self.invalidate_code.get() {
            cpu.exception =
                icicle_cpu::Exception::new(ExceptionCode::Environment, CACHE_INVALIDATED);
        }
    }

    fn execute_lean_block(&mut self, cpu: &mut icicle_cpu::Cpu, address: u64) -> bool {
        if !self.invalidate_code.get() {
            self.check_code_epoch(cpu, address);
        }
        if *self.stop.borrow() {
            cpu.exception.code = ExceptionCode::InstructionLimit as u32;
            cpu.exception.value = address;
            return false;
        }
        if self.invalidate_code.get() {
            cpu.exception =
                icicle_cpu::Exception::new(ExceptionCode::Environment, CACHE_INVALIDATED);
            return false;
        }
        true
    }

    pub fn add_block_hook(&mut self, callback: Box<dyn Fn(u64, u64)>) -> u32 {
        self.block_hooks.add_hook(callback)
    }

    pub fn remove_block_hook(&mut self, id: u32) {
        self.block_hooks.remove_hook(id);
    }

    pub fn add_generic_hook(&mut self, callback: Box<dyn Fn(u64)>) -> u32 {
        self.generic_hooks.add_hook(callback)
    }

    pub fn add_range_hook(&mut self, start: u64, size: u64, callback: Box<dyn Fn(u64)>) -> u32 {
        self.ranged_hooks.add_hook(Box::new(move |address: u64| {
            if size != 0 && is_within_start_and_length(address, start, size) {
                callback(address);
            }
        }))
    }

    pub fn add_specific_hook(&mut self, address: u64, callback: Box<dyn Fn(u64)>) -> u32 {
        let id = self.specific_hooks.add_hook(callback);
        let mapping = self.address_mapping.entry(address).or_insert_with(Vec::new);
        mapping.push(id);
        let (word, bit) = Self::address_filter_bit(address);
        self.address_filter[word] |= bit;
        if let Some(filter) = &self.lean_exact_filter {
            filter.set(self.address_filter);
        }

        return id;
    }

    pub fn schedule(&mut self, callback: Box<dyn Fn()>) {
        self.one_time_callbacks.push(callback);
    }

    pub fn remove_generic_hook(&mut self, id: u32) {
        self.generic_hooks.remove_hook(id);
    }

    pub fn remove_range_hook(&mut self, id: u32) {
        self.ranged_hooks.remove_hook(id);
    }

    pub fn remove_specific_hook(&mut self, id: u32) {
        self.address_mapping.retain(|_, vec| {
            vec.retain(|&x| x != id);
            !vec.is_empty()
        });

        self.specific_hooks.remove_hook(id);
        self.address_filter.fill(0);
        for &address in self.address_mapping.keys() {
            let (word, bit) = Self::address_filter_bit(address);
            self.address_filter[word] |= bit;
        }
        if let Some(filter) = &self.lean_exact_filter {
            filter.set(self.address_filter);
        }
    }
}

pub struct IcicleEmulator {
    executing_thread: std::thread::ThreadId,
    vm: icicle_vm::Vm,
    last_stop: IcicleStopInfo,
    last_vm_exit: icicle_vm::VmExit,
    reg: registers::X86RegisterNodes,
    syscall_hooks: HookContainer<dyn Fn()>,
    timestamp_hooks: [HookContainer<dyn Fn() -> u32>; 2],
    timestamp_epoch: Instant,
    invalidate_code: Rc<Cell<bool>>,
    resume_pcode: bool,
    interrupt_hooks: HookContainer<dyn Fn(i32)>,
    violation_hooks: HookContainer<dyn Fn(u64, u8, bool) -> bool>,
    execution_hooks: Rc<RefCell<ExecutionHooks>>,
    stop: Rc<RefCell<bool>>,
    vm_running: bool,
    exec_write_pending: std::sync::Arc<std::sync::atomic::AtomicBool>,
    exec_write_flushes: std::sync::Arc<std::sync::atomic::AtomicU64>,
    exec_write_wake: Option<std::sync::Arc<icicle_cpu::mem::physical::ExecWriteWake>>,
    invalidation_causes: Rc<Cell<u8>>,
    manual_origins: Cell<u16>,
    range_invalidation_enabled: bool,
    pending_manual_pages: std::collections::HashSet<u64>,
    manual_range_overflow: bool,
    invalidation_profile: Option<std::sync::Arc<InvalidationProfile>>,
    jit_profile_enabled: bool,
    jit_recompile_calls: u64,
    jit_recompile_nanos: u64,
    jit_recompile_compile_calls: u64,
    jit_recompile_compile_nanos: u64,
    jit_reset_generation: u64,
    jit_reset_cause_flags: u64,
    jit_reset_manual_origin_flags: u64,
    jit_flush_code_nanos: u64,
    jit_reset_nanos: u64,
    jit_generation_compile_calls_baseline: u64,
    jit_generation_compile_nanos_baseline: u64,
    pending_free_pages: Vec<icicle_cpu::mem::physical::Index>,
    snapshots: Vec<(
        Box<icicle_vm::Snapshot>,
        Vec<icicle_cpu::mem::physical::Index>,
    )>,
}

impl Drop for IcicleEmulator {
    fn drop(&mut self) {
        assert!(!self.vm_running);
        // JITModule deliberately retains published pointers unless explicitly freed.
        unsafe { self.vm.jit.reset() };
    }
}

struct WriteObservationHook {
    callback: Box<dyn Fn(u64, &[u8], u64, bool)>,
}

impl icicle_cpu::mem::WriteHook for WriteObservationHook {
    fn write(&mut self, _mem: &mut icicle_cpu::Mmu, _addr: u64, _value: &[u8]) {}

    fn write_result(
        &mut self,
        _mem: &mut icicle_cpu::Mmu,
        addr: u64,
        value: &[u8],
        result: icicle_cpu::mem::perm::MemResult<()>,
    ) {
        (self.callback)(
            addr,
            value,
            result.err().map_or(0, |error| error.code()),
            false,
        );
    }

    fn external_write_result(
        &mut self,
        _mem: &mut icicle_cpu::Mmu,
        addr: u64,
        value: &[u8],
        result: icicle_cpu::mem::perm::MemResult<()>,
    ) {
        (self.callback)(
            addr,
            value,
            result.err().map_or(0, |error| error.code()),
            true,
        );
    }
}

struct MemoryHook {
    callback: Box<dyn Fn(u64, &[u8])>,
}

impl icicle_cpu::mem::WriteHook for MemoryHook {
    fn write(&mut self, _mem: &mut icicle_cpu::Mmu, addr: u64, value: &[u8]) {
        (self.callback)(addr, value);
    }
}

impl icicle_cpu::mem::ReadAfterHook for MemoryHook {
    fn read(&mut self, _mem: &mut icicle_cpu::Mmu, addr: u64, value: &[u8]) {
        (self.callback)(addr, value);
    }
}

pub struct MmioHandler {
    read_handler: Box<dyn Fn(u64, &mut [u8])>,
    write_handler: Box<dyn Fn(u64, &[u8])>,
}

impl MmioHandler {
    pub fn new(
        read_function: Box<dyn Fn(u64, &mut [u8])>,
        write_function: Box<dyn Fn(u64, &[u8])>,
    ) -> Self {
        Self {
            read_handler: read_function,
            write_handler: write_function,
        }
    }
}

impl icicle_cpu::mem::IoMemory for MmioHandler {
    fn read(&mut self, addr: u64, buf: &mut [u8]) -> icicle_cpu::mem::MemResult<()> {
        (self.read_handler)(addr, buf);
        return Ok(());
    }

    fn write(&mut self, addr: u64, value: &[u8]) -> icicle_cpu::mem::MemResult<()> {
        (self.write_handler)(addr, value);
        return Ok(());
    }
}

impl IcicleEmulator {
    pub fn new() -> Self {
        let install_instruction_hooks =
            std::env::var("SOGEN_ICICLE_INSTRUCTION_HOOK").map(|v| v != "0").unwrap_or(true);
        Self::new_with_instruction_hooks(install_instruction_hooks)
    }

    fn new_with_instruction_hooks(install_instruction_hooks: bool) -> Self {
        let lean_exact_probe = std::env::var("SOGEN_GUEST_CXX_THROW_PROBE").as_deref() == Ok("1");
        Self::new_with_hook_modes(install_instruction_hooks, lean_exact_probe)
    }

    fn new_with_hook_modes(install_instruction_hooks: bool, lean_exact_probe: bool) -> Self {
        let lean_exact_hooks = lean_exact_probe && !install_instruction_hooks;
        if lean_exact_hooks {
            static REPORTED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
            if !REPORTED.swap(true, std::sync::atomic::Ordering::Relaxed) {
                eprintln!("[SOGEN_ICICLE_LEAN_EXACT_HOOKS_V1] enabled");
            }
        }
        let mut virtual_machine = create_x64_vm();

        let capacity_400mb = 50_000;

        let mut capacity = 8 * 2 * capacity_400mb; // ~8gb
        if cfg!(target_pointer_width = "32") {
            capacity = 2 * capacity_400mb; // ~1gb
        }

        virtual_machine.cpu.mem.set_capacity(capacity);

        let stop_value = Rc::new(RefCell::new(false));
        let invalidate_code = Rc::new(Cell::new(false));
        let invalidation_causes = Rc::new(Cell::new(0));
        let invalidation_profile = (std::env::var("SOGEN_SMP_PROFILE").as_deref() == Ok("1"))
            .then(|| std::sync::Arc::new(InvalidationProfile::default()));
        let mut hook_state = ExecutionHooks::new(stop_value.clone(), invalidate_code.clone());
        let lean_exact_filter = lean_exact_hooks.then(|| Rc::new(Cell::new([0; 4])));
        hook_state.lean_exact_filter = lean_exact_filter.clone();
        hook_state.invalidation_causes = Some(Rc::clone(&invalidation_causes));
        hook_state.invalidation_profile = invalidation_profile.clone();
        let exec_hooks = Rc::new(RefCell::new(hook_state));

        // The per-instruction/per-block hooks drive all instrumentation (call-count, first-exec,
        // execution-progress, coverage, import tracking) but the JIT compiles them as a call on
        // every instruction, a ~5-7x throughput tax (measured; see cpu-test-impl). A lean "fast"
        // run can skip them for pure-JIT speed; the analyzer keeps its syscall/rdtsc/cpuid and
        // memory hooks either way, and correctness is unaffected (handle_instruction is pure
        // observation). Default on; SOGEN_ICICLE_INSTRUCTION_HOOK=0 opts into the fast path
        // (panel: "Instrumentation hooks" -> disabled). Skipping injection also disables
        // per-instruction breakpoints, so the panel pairs this with the debugger being off.
        // Lean epoch hooks require SOGEN_SMP_LEAN_EPOCH_HOOK=1 because their JIT call cost is significant.
        let trace_blocks = ["SOGEN_LEANDIAG_BLOCKTRACE", "SOGEN_LEANDIAG_FULLTRACE"]
            .iter()
            .any(|key| std::env::var(key).map(|v| v == "1").unwrap_or(false));
        // LEAN BARRIER (default on; SOGEN_ICICLE_LEAN_BARRIER=0 opts out): the original sogen
        // ALWAYS injected a hook op per instruction - an opaque call that forces the JIT to
        // spill/reload live pcode temporaries. Lean mode removed them and a deterministic
        // JIT miscompile surfaced (DLL_INIT_FAILED at ~9.4M; interpreter passes - see
        // prompt.md 6.14). Injecting a NO-OP hook per instruction restores the compiler
        // barrier without any callback cost - and keeps the RDTSC rewrite active (icicle's
        // native RDTSC returns a constant 0).
        let lean_barrier =
            std::env::var("SOGEN_ICICLE_LEAN_BARRIER").map(|v| v != "0").unwrap_or(true);
        let lean_epoch_hook = smp_lean_epoch_hook_enabled();
        if install_instruction_hooks || trace_blocks || lean_barrier || lean_epoch_hook || lean_exact_hooks {
            let inst_exec_hooks = Rc::clone(&exec_hooks);

            let inst_hook = icicle_cpu::InstHook::new(move |cpu: &mut icicle_cpu::Cpu, addr: u64| {
                inst_exec_hooks.borrow_mut().execute(cpu, addr);
            });
            // no-op per-instruction hook for lean mode: pure compiler barrier, empty body
            let noop_hook = icicle_cpu::InstHook::new(|_cpu: &mut icicle_cpu::Cpu, _addr: u64| {});
            // Diagnostic-only lean path: preserve the opaque compiler barrier and dispatch only
            // exact-address hooks. Most instructions reject against a VM-local Bloom filter
            // without borrowing ExecutionHooks or running generic/ranged instrumentation.
            let exact_exec_hooks = Rc::clone(&exec_hooks);
            let exact_filter = lean_exact_filter.clone();
            let lean_exact_hook = icicle_cpu::InstHook::new(move |_cpu: &mut icicle_cpu::Cpu, addr: u64| {
                if let Some(filter) = &exact_filter {
                    let (word, bit) = ExecutionHooks::address_filter_bit(addr);
                    if filter.get()[word] & bit != 0 {
                        exact_exec_hooks.borrow_mut().run_specific_hooks(addr);
                    }
                }
            });

            let block_exec_hooks = Rc::clone(&exec_hooks);

            let block_hook = icicle_cpu::InstHook::new(move |cpu: &mut icicle_cpu::Cpu, addr: u64| {
                let instructions = cpu.args[0] as u64;
                let mut hooks = block_exec_hooks.borrow_mut();
                if install_instruction_hooks || !lean_epoch_hook || hooks.execute_lean_block(cpu, addr) {
                    hooks.on_block(addr, instructions);
                }
            });

            let page_exec_hooks = Rc::clone(&exec_hooks);
            let page_hook = icicle_cpu::InstHook::new(move |cpu: &mut icicle_cpu::Cpu, addr: u64| {
                page_exec_hooks.borrow_mut().execute_lean_block(cpu, addr);
            });

            let inst_hook_id = if install_instruction_hooks {
                virtual_machine.cpu.add_hook(inst_hook)
            } else if lean_exact_hooks {
                virtual_machine.cpu.add_hook(lean_exact_hook)
            } else {
                // lean: noop barrier (also when trace_blocks set the block-only mode ran
                // before; the barrier covers it - the RDTSC rewrite stays identical)
                virtual_machine.cpu.add_hook(noop_hook)
            };
            // Opt-in: keep the lean fence but lower this proven-empty hook to a direct
            // opaque JIT call without hook-table loads; keep the exception check.
            if !install_instruction_hooks
                && !lean_exact_hooks
                && lean_barrier
                && std::env::var("SOGEN_ICICLE_FAST_NOOP_BARRIER").as_deref() == Ok("1")
            {
                virtual_machine.jit.set_noop_barrier_hook(inst_hook_id);
            }
            let block_hook_id = virtual_machine.cpu.add_hook(block_hook);
            let page_hook_id = virtual_machine.cpu.add_hook(page_hook);
            // Without the lean barrier, epoch checks run at block starts and page transitions.
            let block_only = !install_instruction_hooks && !lean_barrier && !lean_exact_hooks;
            virtual_machine.add_injector(InstructionHookInjector {
                inst_hook: inst_hook_id,
                block_hook: block_hook_id,
                page_hook: page_hook_id,
                emit_block_hook: install_instruction_hooks || trace_blocks || lean_epoch_hook,
                check_page_transitions: !install_instruction_hooks && lean_epoch_hook,
                block_only,
            });
        }

        Self {
            stop: stop_value,
            executing_thread: std::thread::current().id(),
            last_stop: IcicleStopInfo::none(),
            last_vm_exit: icicle_vm::VmExit::Running,
            reg: registers::X86RegisterNodes::new(&virtual_machine.cpu.arch),
            vm: virtual_machine,
            syscall_hooks: HookContainer::new(),
            timestamp_hooks: [HookContainer::new(), HookContainer::new()],
            timestamp_epoch: Instant::now(),
            invalidate_code,
            resume_pcode: false,
            interrupt_hooks: HookContainer::new(),
            violation_hooks: HookContainer::new(),
            execution_hooks: exec_hooks,
            vm_running: false,
            exec_write_pending: std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false)),
            exec_write_flushes: std::sync::Arc::new(std::sync::atomic::AtomicU64::new(0)),
            exec_write_wake: None,
            invalidation_causes,
            manual_origins: Cell::new(0),
            range_invalidation_enabled: range_invalidation_enabled(),
            pending_manual_pages: std::collections::HashSet::new(),
            manual_range_overflow: false,
            invalidation_profile,
            jit_profile_enabled: std::env::var("SOGEN_ICICLE_JIT_PROFILE").as_deref() == Ok("1")
                || std::env::var("SOGEN_ICICLE_COMPILE_ORIGIN_PROFILE").as_deref() == Ok("1"),
            jit_recompile_calls: 0,
            jit_recompile_nanos: 0,
            jit_recompile_compile_calls: 0,
            jit_recompile_compile_nanos: 0,
            jit_reset_generation: 0,
            jit_reset_cause_flags: 0,
            jit_reset_manual_origin_flags: 0,
            jit_flush_code_nanos: 0,
            jit_reset_nanos: 0,
            jit_generation_compile_calls_baseline: 0,
            jit_generation_compile_nanos_baseline: 0,
            pending_free_pages: Vec::new(),
            snapshots: Vec::new(),
        }
    }

    pub fn set_memory_limit_mib(&mut self, mib: u64) -> bool {
        let Some(pages) = mib.checked_mul(256) else {
            return false;
        };
        if mib == 0 || pages > u32::MAX as u64 {
            return false;
        }
        let Ok(pages) = usize::try_from(pages) else {
            return false;
        };
        self.vm.cpu.mem.set_capacity(pages)
    }

    fn get_mem(&mut self) -> &mut icicle_vm::cpu::Mmu {
        return &mut self.vm.cpu.mem;
    }

    pub(crate) fn stop_flag(&self) -> std::sync::Arc<std::sync::atomic::AtomicBool> {
        std::sync::Arc::clone(&self.vm.interrupt_flag)
    }

    pub(crate) fn exec_write_flushes_flag(&self) -> std::sync::Arc<std::sync::atomic::AtomicU64> {
        std::sync::Arc::clone(&self.exec_write_flushes)
    }

    pub(crate) fn exec_write_pending_flag(&self) -> std::sync::Arc<std::sync::atomic::AtomicBool> {
        std::sync::Arc::clone(&self.exec_write_pending)
    }

    /// Called only during machine setup, before shared pages or worker threads exist.
    pub(crate) fn link_exec_write_wake(
        &mut self,
        wake: std::sync::Arc<icicle_cpu::mem::physical::ExecWriteWake>,
    ) {
        self.vm.cpu.mem.set_exec_write_wake(std::sync::Arc::clone(&wake));
        self.exec_write_wake = Some(wake);
    }

    pub(crate) fn invalidation_profile_flag(&self) -> Option<std::sync::Arc<InvalidationProfile>> {
        self.invalidation_profile.clone()
    }

    fn mark_invalidation_cause(&self, cause: u8) {
        self.invalidation_causes.set(self.invalidation_causes.get() | cause);
    }

    fn mark_manual_origin(&self, origin: u16) {
        self.invalidation_causes.set(self.invalidation_causes.get() | INVALIDATION_MANUAL);
        self.manual_origins.set(self.manual_origins.get() | origin);
    }

    fn record_manual_page(&mut self, address: u64) {
        const MAX_PENDING_PAGES: usize = 1024;
        if !self.range_invalidation_enabled {
            return;
        }
        if self.pending_manual_pages.len() < MAX_PENDING_PAGES {
            self.pending_manual_pages.insert(address);
        } else if !self.pending_manual_pages.contains(&address) {
            self.manual_range_overflow = true;
        }
    }

    pub(crate) fn exec_write_pending(&self) -> bool {
        self.exec_write_pending.load(std::sync::atomic::Ordering::Acquire)
    }

    /// Owner thread only, after a JIT exit and before guest execution resumes.
    pub(crate) fn reconcile_exec_write_wake(&mut self) {
        if self.exec_write_pending.swap(false, std::sync::atomic::Ordering::AcqRel) {
            self.resume_pcode = false;
            self.mark_invalidation_cause(INVALIDATION_WAKE);
            self.invalidate_code.set(true);
            self.flush_pending_code();
            self.exec_write_flushes.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        }
    }

    pub(crate) fn owner_stop_cell(&self) -> Rc<RefCell<bool>> {
        Rc::clone(&self.stop)
    }

    pub fn start(&mut self, count: u64) {
        self.executing_thread = std::thread::current().id();
        if self.range_invalidation_enabled {

            // Reclaim retained dead code at a parked owner boundary. Never free a JIT module
            // while a generated-code frame or resumed p-code operation can still refer to it.
            if !self.resume_pcode && self.range_cache_over_limit() {
                self.invalidate_code.set(true);
            }
        }
        self.last_stop = IcicleStopInfo::none();
        self.last_vm_exit = icicle_vm::VmExit::Running;
        // A peer may stop us after the scheduler marked this VM active but before
        // vm.run(). Consume that request instead of clearing and losing it.
        if self.vm.interrupt_flag.swap(false, std::sync::atomic::Ordering::AcqRel) {
            self.last_vm_exit = icicle_vm::VmExit::Interrupted;
            self.last_stop = IcicleStopInfo::instruction_limit();
            return;
        }

        self.vm.icount_limit = match count {
            0 => u64::MAX,
            _ => self.vm.cpu.icount.saturating_add(count),
        };

        loop {
            if !std::mem::take(&mut self.resume_pcode) {
                self.flush_pending_code();
                self.reclaim_pending_pages();
                self.vm.cpu.block_id = u64::MAX;
                self.vm.cpu.block_offset = 0;
            }
            self.vm.cpu.pending_exception = None;
            self.vm.cpu.exception.clear();
            *self.stop.borrow_mut() = false;

            // Vm::run() normally recompiles as its first action. Time that same
            // boundary only when profiling, leaving the default path unchanged.
            if self.jit_profile_enabled && self.vm.enable_recompilation && self.vm.should_recompile() {
                let compile_calls = self.vm.jit.profile_compile_calls;
                let compile_nanos = self.vm.jit.profile_compile_nanos;
                let start = Instant::now();
                self.vm.jit.compile_origin_recompile_active = true;
                self.vm.recompile();
                self.vm.jit.compile_origin_recompile_active = false;
                self.jit_recompile_calls += 1;
                self.jit_recompile_nanos = self.jit_recompile_nanos.saturating_add(
                    start.elapsed().as_nanos().min(u64::MAX as u128) as u64,
                );
                self.jit_recompile_compile_calls += self.vm.jit.profile_compile_calls - compile_calls;
                self.jit_recompile_compile_nanos += self.vm.jit.profile_compile_nanos - compile_nanos;
            }
            self.vm_running = true;
            let wake_guard = self.exec_write_wake.as_ref().map(|wake| wake.run_guard());
            let reason = self.vm.run();
            drop(wake_guard);
            self.vm_running = false;
            self.last_vm_exit = reason;

            // SMP 6.7: a locked RMW section can never legitimately span a VM exit - if the block
            // ended between the LOCK and UNLOCK hooks (fault, kick, icount edge), release the RMW
            // spinlock so peers never spin on a dead holder (post-6.7 probe showed 120s hangs).
            smp_rmw_lock::release();

            match reason {
                icicle_vm::VmExit::InstructionLimit => {
                    self.last_stop = IcicleStopInfo::instruction_limit();
                    break;
                }
                icicle_vm::VmExit::Interrupted => {
                    self.last_stop = IcicleStopInfo::instruction_limit();
                    break;
                }
                icicle_vm::VmExit::UnhandledException((code, value)) => {
                    let continue_execution = self.handle_exception(code, value);
                    // An interrupt or exception hook can request a stop on this
                    // owner thread. Honor it before the next loop iteration clears
                    // the hook stop cell; otherwise an int29 fast-fail repeats forever.
                    if *self.stop.borrow() {
                        self.last_stop = IcicleStopInfo::instruction_limit();
                        break;
                    }
                    if !continue_execution {
                        self.last_stop = IcicleStopInfo::unhandled_exception(code, value);
                        break;
                    }
                }
                _ => {
                    self.last_stop = IcicleStopInfo::other(
                        self.vm.cpu.exception.code,
                        self.vm.cpu.exception.value,
                    );
                    break;
                }
            };
        }
        // The VM is parked. A remote stop may have arrived alongside another
        // exit; C++ retains the matching quiesce, kick, or real-stop state.
        // Do not let that interrupt leak into the next run quantum.
        self.vm.interrupt_flag.store(false, std::sync::atomic::Ordering::Release);
    }

    pub fn vm_exit_description(&self) -> String {
        format!(
            "{:?}; physical_pages={}/{}; pending_free_pages={}",
            self.last_vm_exit,
            self.vm.cpu.mem.total_pages(),
            self.vm.cpu.mem.capacity(),
            self.pending_free_pages.len()
        )
    }

    pub fn last_stop_info(&self) -> IcicleStopInfo {
        return self.last_stop;
    }

    fn handle_interrupt(&mut self, code: i32) -> bool {
        self.interrupt_hooks.for_each_hook(|func| {
            func(code);
        });

        return true;
    }

    fn handle_exception(&mut self, code: ExceptionCode, value: u64) -> bool {
        smp_rmw_lock::release(); // 6.7: faulted mid-locked-section - never leave the RMW spinlock held
        // The analyzer already records syscalls and faults. Keep this per-exception Rust echo opt-in:
        // it allocates a format string and writes stderr on every syscall in busy guest loops.
        static EXCEPTION_DEBUG: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        if *EXCEPTION_DEBUG.get_or_init(|| std::env::var("SOGEN_SMP_EXCEPTION_DEBUG").as_deref() == Ok("1")) {
            smp_dbg(&format!("handle_exception code={code:?} value={value:#x} pc={:#x}", self.vm.cpu.read_pc()));
        }
        let continue_execution = match code {
            ExceptionCode::Syscall => self.handle_syscall(value),
            ExceptionCode::ReadPerm => self.handle_violation(value, FOREIGN_READ, false),
            ExceptionCode::WritePerm => self.handle_violation(value, FOREIGN_WRITE, false),
            ExceptionCode::ReadUnmapped => self.handle_violation(value, FOREIGN_READ, true),
            ExceptionCode::WriteUnmapped => self.handle_violation(value, FOREIGN_WRITE, true),
            ExceptionCode::ExecViolation => self.handle_execute_violation(value),
            ExceptionCode::SelfModifyingCode => self.handle_self_modifying_code(value),
            ExceptionCode::Environment => self.handle_environment(value),
            ExceptionCode::SoftwareBreakpoint => self.handle_interrupt(3),
            ExceptionCode::InvalidInstruction => self.handle_interrupt(6),
            ExceptionCode::DivisionException => self.handle_interrupt(0),
            _ => false,
        };

        return continue_execution;
    }

    /// SMP 6.6: drop this VM's translations/TLB for [address, address+length). Returns whether
    /// anything was actually invalidated (an executed page was touched). No-op for uncached ranges.
    pub fn invalidate_code_range_public(&mut self, address: u64, length: u64) -> bool {
        let changed = self.invalidate_code_range(address, length, MANUAL_PUBLIC_INVALIDATE);
        if changed {
            // invalidate_code_range already cleared the TLB and set the flag; nothing extra needed.
        }
        return changed;
    }

    /// A peer's shared PageData permissions were already changed by the issuing VM. Its own
    /// translated code and data TLB can still cache the old access rights. Refresh local state
    /// without touching shared permission bytes or their global IN_CODE_CACHE marker: another
    /// vCPU may still be executing translated code from the same page.
    pub fn refresh_peer_protection(&mut self, address: u64, length: u64) {
        if length == 0 {
            return;
        }
        let last = address.saturating_add(length - 1);
        let page_size = self.vm.cpu.mem.page_size();
        let mut page_address = self.vm.cpu.mem.page_aligned(address);
        let mut code_changed = false;
        loop {
            if let Some(index) = self.vm.cpu.mem.get_physical_index(page_address) {
                let page = self.vm.cpu.mem.get_physical_mut(index);
                if page.executed {
                    page.executed = false;
                    code_changed = true;
                    self.record_manual_page(page_address);
                }
            }
            if last - page_address < page_size {
                break;
            }
            page_address += page_size;
        }
        self.vm.cpu.mem.clear_tlb();
        if code_changed {
            self.mark_manual_origin(MANUAL_PEER_PROTECTION);
            self.invalidate_code.set(true);
        }
    }

    /// SMP 6.6: read-only query — does [address, address+length) overlap an EXECUTED (translated)
    /// page in this VM? Lets the C++ backend decide whether a host write needs peer fan-out,
    /// without mutating anything.
    /// SMP 6.6c'': the SMALLEST perm epoch over the range's pages (0 if unmapped). The backend
    /// captures this when QUEUEING a deferred protect and skips applying if it changed (the
    /// page was re-mapped/re-protected since - a stale protect must not land over newer perms).
    pub fn perm_epoch_of_range(&self, address: u64, length: u64) -> u64 {
        if length == 0 {
            return 0;
        }
        let last = address.saturating_add(length - 1);
        let mem = &self.vm.cpu.mem;
        let page_size = mem.page_size();
        let mut page_address = mem.page_aligned(address);
        let mut min_epoch = u64::MAX;
        loop {
            if let Some(index) = mem.get_physical_index(page_address) {
                let page = mem.get_physical(index);
                if page.smp_shared {
                    min_epoch = min_epoch.min(page.data().perm_epoch());
                } else {
                    return 0; // non-shared page: no cross-VM ordering concern
                }
            }
            if last - page_address < page_size {
                break;
            }
            page_address += page_size;
        }
        if min_epoch == u64::MAX { 0 } else { min_epoch }
    }

    pub fn code_range_is_cached(&self, address: u64, length: u64) -> bool {
        use icicle_vm::cpu::mem::perm;
        if length == 0 {
            return false;
        }
        let last = address.saturating_add(length - 1);
        let mem = &self.vm.cpu.mem;
        let page_size = mem.page_size();
        let mut page_address = mem.page_aligned(address);
        loop {
            if let Some(index) = mem.get_physical_index(page_address) {
                let page = mem.get_physical(index);
                if page.executed {
                    return true; // this VM translated the page
                }
                if page.smp_shared {
                    // The shared PageData perms carry IN_CODE_CACHE set by ANY sharing VM's
                    // translation (ensure_executable mutates them in place since the write_ptr
                    // fix), so this is a cross-VM-visible "someone translated this" signal.
                    let data = page.data();
                    let start = ((page_address.max(address) - page_address) & (page_size - 1)) as usize;
                    let end = (((last.min(page_address + page_size - 1)) - page_address) & (page_size - 1)) as usize;
                    if data.any_perm_bit(start, end - start + 1, perm::IN_CODE_CACHE) {
                        return true;
                    }
                }
            }
            if last - page_address < page_size {
                break;
            }
            page_address += page_size;
        }
        return false;
    }

    pub fn host_view_code_may_be_cached(&self, address: u64, length: u64) -> bool {
        use icicle_vm::cpu::mem::perm;
        use std::sync::atomic::Ordering;

        if length == 0 {
            return false;
        }
        let last = address.saturating_add(length - 1);
        let mem = &self.vm.cpu.mem;
        let page_size = mem.page_size();
        let mut page_address = mem.page_aligned(address);
        loop {
            if let Some(index) = mem.get_physical_index(page_address) {
                let page = mem.get_physical(index);
                if page.smp_shared {
                    if page.data().smp_translated.load(Ordering::Acquire) != 0 {
                        return true;
                    }
                } else if mem.get_perm(page_address) & perm::EXEC != 0 {
                    return true;
                }
            }
            if last - page_address < page_size {
                break;
            }
            page_address += page_size;
        }
        false
    }

    fn invalidate_code_range(&mut self, address: u64, length: u64, origin: u16) -> bool {
        if length == 0 {
            return false;
        }
        let last = address.saturating_add(length - 1);
        let page_size = self.vm.cpu.mem.page_size();
        let mut page_address = self.vm.cpu.mem.page_aligned(address);
        let mut changed = false;
        loop {
            if let Some(index) = self.vm.cpu.mem.get_physical_index(page_address.max(address)) {
                let page = self.vm.cpu.mem.get_physical_mut(index);
                if page.executed {
                    page.executed = false;
                    if !page.smp_shared {
                        for permission in &mut page.data_mut().perm {
                            *permission &= !icicle_cpu::mem::perm::IN_CODE_CACHE;
                        }
                    }
                    // A peer may still have translated shared code; keep its marker.
                    changed = true;
                    self.record_manual_page(page_address);
                }
            }
            if last - page_address < page_size {
                break;
            }
            page_address += page_size;
        }
        if changed {
            self.vm.cpu.mem.clear_tlb();
            self.mark_manual_origin(origin);
            self.invalidate_code.set(true);
        }
        changed
    }

    fn range_cache_over_limit(&self) -> bool {
        const MAX_RETAINED_HOST_CODE: u64 = 256 * 1024 * 1024;
        const MAX_LIFTED_BLOCKS: usize = 250_000;
        self.vm.jit.generated_code_bytes() >= MAX_RETAINED_HOST_CODE
            || self.vm.code.blocks.len() >= MAX_LIFTED_BLOCKS
    }

    /// Remove lifted groups that touch a dirty guest page. Retain the generated-code module:
    /// old pointers remain allocated until the owning VM is parked for a later full purge.
    fn invalidate_pending_manual_pages(&mut self) {
        let pages = std::mem::take(&mut self.pending_manual_pages);
        let page_size = self.vm.cpu.mem.page_size();
        let blocks = &self.vm.code.blocks;
        let mut invalidated = std::collections::HashSet::new();
        self.vm.code.map.retain(|_, group| {
            let overlaps = group.range().any(|id| {
                let block = &blocks[id];
                let mut page = block.start & !(page_size - 1);
                let last = block.end.saturating_sub(1).max(block.start) & !(page_size - 1);
                loop {
                    if pages.contains(&page) {
                        return true;
                    }
                    if page >= last {
                        return false;
                    }
                    page = match page.checked_add(page_size) {
                        Some(next) => next,
                        None => return false,
                    };
                }
            });
            if overlaps {
                invalidated.extend(group.range());
            }
            !overlaps
        });
        // A lifted group can start on a clean page and extend into a dirty one. The
        // disassembly cache has no reverse group index, so clear its diagnostic strings.
        self.vm.code.disasm.clear();
        self.vm.code.modified.retain(|id| !invalidated.contains(id));
        // Vm::recompile seeds its traversal from every retained block with an entry.
        // Removing the group from code.map alone lets an old entry recompile stale p-code.
        // Tombstone every block in the removed group, including any internal entry, while
        // retaining its storage until the bounded full reset. External edges from live
        // groups resolve through code.map; their internal edges remain group-local.
        for id in invalidated {
            self.vm.code.blocks[id].entry = None;
            self.vm.jit.invalidate(id);
        }
        self.vm.cpu.block_id = u64::MAX;
        self.vm.cpu.block_offset = 0;
    }

    fn flush_pending_code(&mut self) {
        if self.invalidate_code.replace(false) {
            assert!(!self.vm_running);
            let causes = self.invalidation_causes.replace(0);
            let manual_origins = self.manual_origins.replace(0);
            if self.range_invalidation_enabled
                && causes == INVALIDATION_MANUAL
                && !self.pending_manual_pages.is_empty()
                && !self.manual_range_overflow
                && !self.resume_pcode
                && !self.range_cache_over_limit()
            {
                self.invalidate_pending_manual_pages();
                return;
            }
            self.pending_manual_pages.clear();
            self.manual_range_overflow = false;
            smp_dbg("flush_pending_code: flushing code+vising jit");
            if let Some(profile) = &self.invalidation_profile {
                profile.record_reset(causes, manual_origins);
            }
            let flush_start = self.jit_profile_enabled.then(Instant::now);
            self.vm.code.flush_code();
            let reset_start = flush_start.map(|start| {
                self.jit_flush_code_nanos = self.jit_flush_code_nanos.saturating_add(
                    start.elapsed().as_nanos().min(u64::MAX as u128) as u64,
                );
                Instant::now()
            });
            // Vm::run has returned, so no generated-code frame still references this module.
            unsafe { self.vm.jit.reset() };
            if let Some(start) = reset_start {
                self.jit_reset_nanos = self.jit_reset_nanos.saturating_add(
                    start.elapsed().as_nanos().min(u64::MAX as u128) as u64,
                );
                self.jit_reset_generation = self.jit_reset_generation.saturating_add(1);
                self.jit_reset_cause_flags = causes as u64;
                self.jit_reset_manual_origin_flags = manual_origins as u64;
                self.jit_generation_compile_calls_baseline = self.vm.jit.profile_compile_calls;
                self.jit_generation_compile_nanos_baseline = self.vm.jit.profile_compile_nanos;
            }
            self.vm.cpu.block_id = u64::MAX;
            self.vm.cpu.block_offset = 0;
        }
    }

    fn handle_self_modifying_code(&mut self, address: u64) -> bool {
        let length = self
            .vm
            .code
            .blocks
            .get(self.vm.cpu.block_id as usize)
            .and_then(|block| {
                block
                    .pcode
                    .instructions
                    .get(self.vm.cpu.block_offset as usize)
            })
            .filter(|instruction| matches!(instruction.op, pcode::Op::Store(_)))
            .map_or(1, |instruction| instruction.inputs.second().size() as u64);
        if !self.invalidate_code_range(address, length, MANUAL_SELF_MODIFYING) {
            return false;
        }
        // Preserve the failed p-code operation: replaying the x86 instruction can duplicate earlier stores or stack updates.
        self.resume_pcode = self.vm.cpu.block_id != u64::MAX;
        true
    }

    fn handle_environment(&mut self, value: u64) -> bool {
        if value & !0xff == crate::packed_sad::ARCHITECTURAL_FAULT {
            return self.handle_interrupt((value & 0xff) as i32);
        }
        if value == CACHE_INVALIDATED {
            smp_dbg("handle_environment CACHE_INVALIDATED -> flush");
            self.flush_pending_code();
            smp_dbg("handle_environment flushed, continue");
            return true;
        }
        if value == crate::xstate::GENERAL_PROTECTION {
            return self.handle_interrupt(13);
        }
        let kind = value >> 8;
        if !(1..=2).contains(&kind) {
            return false;
        }
        let mut continuation = 0;
        self.timestamp_hooks[(kind - 1) as usize].for_each_hook(|callback| {
            continuation = continuation.max(callback());
        });
        if continuation == 0 {
            let ticks = self.timestamp_epoch.elapsed().as_nanos() as u64;
            self.write_register(
                registers::X86Register::Rax,
                &(ticks as u32 as u64).to_ne_bytes(),
            );
            self.write_register(
                registers::X86Register::Rdx,
                &((ticks >> 32) as u32 as u64).to_ne_bytes(),
            );
            if kind == 2 {
                self.write_register(registers::X86Register::Rcx, &0u64.to_ne_bytes());
            }
        }
        if continuation != 2 {
            self.vm.cpu.write_pc(self.vm.cpu.read_pc() + (value & 0xff));
        }
        true
    }

    pub fn add_timestamp_hook(&mut self, serializing: bool, callback: Box<dyn Fn() -> u32>) -> u32 {
        let id = self.timestamp_hooks[serializing as usize].add_hook(callback);
        qualify_hook_id(
            id,
            if serializing {
                HookType::TimestampSerializing
            } else {
                HookType::Timestamp
            },
        )
    }

    fn handle_execute_violation(&mut self, instruction: u64) -> bool {
        use icicle_cpu::mem::perm;

        // Icicle reports the instruction start, even when a later instruction byte fails the fetch.
        let required = perm::EXEC | perm::INIT;
        let address = (0..15)
            .filter_map(|offset| instruction.checked_add(offset))
            .find(|address| self.vm.cpu.mem.get_perm(*address) & required != required)
            .unwrap_or(instruction);
        let unmapped = self.vm.cpu.mem.get_perm(address) & perm::MAP == 0;
        self.handle_violation(address, FOREIGN_EXEC, unmapped)
    }

    fn handle_violation(&mut self, address: u64, permission: u8, unmapped: bool) -> bool {
        // SMP 6.6: trace every ENTRY so the terminal-fault delivery path is identifiable
        // (failing probe runs show restarted=0/declined=0 - some faults never reach the
        // C++ wrapper; printing here catches whichever icicle path delivers them).
        if std::env::var("SOGEN_SMP_TRACE").map(|v| v == "1").unwrap_or(false) {
            let pc = self.vm.cpu.read_pc();
            eprintln!(
                "[VIENTRY] addr={address:#x} perm={permission:#x} unmapped={unmapped} pc={pc:#x} hooks_empty={} tid={:?}",
                self.violation_hooks.is_empty(),
                std::thread::current().id()
            );
        }
        if self.violation_hooks.is_empty() {
            return false;
        }

        let mut continue_execution = true;

        self.violation_hooks.for_each_hook(|func| {
            continue_execution &= func(address, permission, unmapped);
        });

        return continue_execution;
    }

    fn handle_syscall(&mut self, value: u64) -> bool {
        if value != 0 {
            return self.handle_interrupt(value as i32);
        }

        // Opt-in, bounded diagnostic for a syscall exit whose reported PC may
        // be stale after JIT execution. The instruction marker comes from the
        // pcode block that actually raised the exception.
        static SOURCE_PROBE: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        if *SOURCE_PROBE.get_or_init(|| std::env::var("SOGEN_SYSCALL_SOURCE_PROBE").as_deref() == Ok("1")) {
            use std::sync::atomic::{AtomicUsize, Ordering};
            static FIRST_SAMPLES: AtomicUsize = AtomicUsize::new(0);
            static MISMATCH_SAMPLES: AtomicUsize = AtomicUsize::new(0);
            static POWER_ID_SAMPLES: AtomicUsize = AtomicUsize::new(0);

            let pc = self.vm.cpu.read_pc();
            let block_id = self.vm.cpu.block_id;
            let block_offset = self.vm.cpu.block_offset;
            let marker = self.vm.code.blocks.get(block_id as usize).and_then(|block| {
                block.pcode.instructions.get(..=block_offset as usize)?.iter().rev()
                    .find(|stmt| matches!(stmt.op, pcode::Op::InstructionMarker))
                    .map(|stmt| stmt.inputs.first().as_u64())
            });
            let mut eax_bytes = [0u8; 4];
            self.read_register(registers::X86Register::Eax, &mut eax_bytes);
            let eax = u32::from_le_bytes(eax_bytes);
            let first = FIRST_SAMPLES.fetch_add(1, Ordering::Relaxed) < 8;
            let mismatch = marker.is_some_and(|address| address != pc)
                && MISMATCH_SAMPLES.fetch_add(1, Ordering::Relaxed) < 32;
            // 0x102 is both NtInitiatePowerAction's ID in this ntdll and the
            // Win32 WAIT_TIMEOUT result seen after WaitForMultipleObjectsEx.
            let power_id = eax == 0x102 && POWER_ID_SAMPLES.fetch_add(1, Ordering::Relaxed) < 8;
            if first || mismatch || power_id {
                eprintln!(
                    "[SYSCALL_SOURCE] code={:#x} value={value:#x} pc={pc:#x} block={block_id:#x} offset={block_offset:#x} marker={marker:?} eax={eax:#x} host_thread={:?}",
                    self.vm.cpu.exception.code,
                    std::thread::current().id()
                );
            }
        }

        self.syscall_hooks.for_each_hook(|func| {
            func();
        });

        self.vm.cpu.write_pc(self.vm.cpu.read_pc() + 2);
        return true;
    }

    pub fn icount(&self) -> u64 {
        return self.vm.cpu.icount;
    }

    pub fn jit_profile(&self) -> IcicleJitProfile {
        IcicleJitProfile {
            compile_calls: self.vm.jit.profile_compile_calls,
            compile_nanos: self.vm.jit.profile_compile_nanos,
            reset_calls: self.vm.jit.profile_reset_calls,
            recompile_calls: self.jit_recompile_calls,
            recompile_nanos: self.jit_recompile_nanos,
            recompile_compile_calls: self.jit_recompile_compile_calls,
            recompile_compile_nanos: self.jit_recompile_compile_nanos,
            reset_generation: self.jit_reset_generation,
            reset_cause_flags: self.jit_reset_cause_flags,
            reset_manual_origin_flags: self.jit_reset_manual_origin_flags,
            flush_code_nanos: self.jit_flush_code_nanos,
            jit_reset_nanos: self.jit_reset_nanos,
            generation_compile_calls: self.vm.jit.profile_compile_calls
                .saturating_sub(self.jit_generation_compile_calls_baseline),
            generation_compile_nanos: self.vm.jit.profile_compile_nanos
                .saturating_sub(self.jit_generation_compile_nanos_baseline),
            origin_first_address_compiles: self.vm.jit.compile_origin.first_address_compiles,
            origin_repeat_after_reset_compiles: self.vm.jit.compile_origin.repeat_after_reset_compiles,
            origin_repeat_in_generation_compiles: self.vm.jit.compile_origin.repeat_in_generation_compiles,
            origin_periodic_recompile_compiles: self.vm.jit.compile_origin.periodic_recompile_compiles,
            origin_unclassified_compiles: self.vm.jit.compile_origin.unclassified_compiles,
            origin_generation_number: self.vm.jit.compile_origin.generation_number,
        }
    }

    pub fn add_block_hook(&mut self, callback: Box<dyn Fn(u64, u64)>) -> u32 {
        let hook_id = self.execution_hooks.borrow_mut().add_block_hook(callback);
        return qualify_hook_id(hook_id, HookType::Block);
    }

    pub fn add_violation_hook(&mut self, callback: Box<dyn Fn(u64, u8, bool) -> bool>) -> u32 {
        let hook_id = self.violation_hooks.add_hook(callback);
        return qualify_hook_id(hook_id, HookType::Violation);
    }

    pub fn add_execution_hook(&mut self, address: u64, callback: Box<dyn Fn(u64)>) -> u32 {
        let hook_id = self
            .execution_hooks
            .borrow_mut()
            .add_specific_hook(address, callback);
        return qualify_hook_id(hook_id, HookType::ExecuteSpecific);
    }

    pub fn add_generic_execution_hook(&mut self, callback: Box<dyn Fn(u64)>) -> u32 {
        let hook_id = self.execution_hooks.borrow_mut().add_generic_hook(callback);
        return qualify_hook_id(hook_id, HookType::ExecuteGeneric);
    }

    pub fn add_ranged_execution_hook(
        &mut self,
        start: u64,
        size: u64,
        callback: Box<dyn Fn(u64)>,
    ) -> u32 {
        let hook_id = self
            .execution_hooks
            .borrow_mut()
            .add_range_hook(start, size, callback);
        return qualify_hook_id(hook_id, HookType::ExecuteRange);
    }

    pub fn add_syscall_hook(&mut self, callback: Box<dyn Fn()>) -> u32 {
        let hook_id = self.syscall_hooks.add_hook(callback);
        return qualify_hook_id(hook_id, HookType::Syscall);
    }

    pub fn add_interrupt_hook(&mut self, callback: Box<dyn Fn(i32)>) -> u32 {
        let hook_id = self.interrupt_hooks.add_hook(callback);
        return qualify_hook_id(hook_id, HookType::Interrupt);
    }

    pub fn add_read_hook(
        &mut self,
        start: u64,
        end: u64,
        callback: Box<dyn Fn(u64, &[u8])>,
    ) -> u32 {
        let id = self
            .get_mem()
            .add_read_after_hook(start, end, Box::new(MemoryHook { callback }));
        if id.is_none() {
            return 0;
        }

        return qualify_hook_id(id.unwrap(), HookType::Read);
    }

    pub fn add_write_hook(
        &mut self,
        start: u64,
        end: u64,
        callback: Box<dyn Fn(u64, &[u8])>,
    ) -> u32 {
        let id = self
            .get_mem()
            .add_write_hook(start, end, Box::new(MemoryHook { callback }));
        if id.is_none() {
            return 0;
        }

        return qualify_hook_id(id.unwrap(), HookType::Write);
    }

    pub fn add_write_observation_hook(
        &mut self,
        start: u64,
        end: u64,
        callback: Box<dyn Fn(u64, &[u8], u64, bool)>,
    ) -> u32 {
        let Some(id) =
            self.get_mem()
                .add_write_hook(start, end, Box::new(WriteObservationHook { callback }))
        else {
            return 0;
        };
        qualify_hook_id(id, HookType::Write)
    }

    pub fn remove_hook(&mut self, id: u32) {
        let (hook_id, hook_type) = split_hook_id(id);

        match hook_type {
            HookType::Syscall => self.syscall_hooks.remove_hook(hook_id),
            HookType::Timestamp => self.timestamp_hooks[0].remove_hook(hook_id),
            HookType::TimestampSerializing => self.timestamp_hooks[1].remove_hook(hook_id),
            HookType::Violation => self.violation_hooks.remove_hook(hook_id),
            HookType::Interrupt => self.interrupt_hooks.remove_hook(hook_id),
            HookType::ExecuteGeneric => self
                .execution_hooks
                .borrow_mut()
                .remove_generic_hook(hook_id),
            HookType::ExecuteSpecific => self
                .execution_hooks
                .borrow_mut()
                .remove_specific_hook(hook_id),
            HookType::ExecuteRange => self.execution_hooks.borrow_mut().remove_range_hook(hook_id),
            HookType::Block => self.execution_hooks.borrow_mut().remove_block_hook(hook_id),
            HookType::Read => {
                self.get_mem().remove_read_after_hook(hook_id);
                ()
            }
            HookType::Write => {
                self.get_mem().remove_write_hook(hook_id);
                ()
            }
            _ => {}
        }
    }

    pub fn run_on_next_instruction(&mut self, callback: Box<dyn Fn()>) {
        self.execution_hooks.borrow_mut().schedule(callback);
    }

    pub fn map_memory(&mut self, address: u64, length: u64, permissions: u8) -> bool {
        const MAPPING_PERMISSIONS: u8 =
            icicle_vm::cpu::mem::perm::MAP | icicle_vm::cpu::mem::perm::INIT;

        let native_permissions = map_permissions(permissions);

        let mapping = icicle_vm::cpu::mem::Mapping {
            perm: native_permissions | MAPPING_PERMISSIONS,
            value: 0x0,
        };

        return self.get_mem().map_memory_len(address, length, mapping);
    }

    pub unsafe fn map_host_memory(&mut self, address: u64, pointer: *mut u8, length: u64, permissions: u8) -> bool {
        unsafe { self.get_mem().map_host_memory(address, pointer, length, map_permissions(permissions)) }
    }

    pub fn has_host_mappings(&self) -> bool {
        self.vm.cpu.mem.has_host_mappings()
    }

    pub fn flush_host_memory_cache(&mut self, pointer: usize, length: usize) {
        for (address, size) in self.vm.cpu.mem.host_mapping_aliases(pointer, length) {
            self.invalidate_code_range(address, size, MANUAL_HOST_CACHE);
        }
    }

    pub fn map_shared_memory(
        &mut self,
        address: u64,
        source: u64,
        length: u64,
        permissions: u8,
    ) -> bool {
        self.get_mem()
            .map_shared(address, source, length, map_permissions(permissions))
            .is_ok()
    }

    /// Map a range of fresh SMP-shared pages on this VM (the master, vCPU 0). Each page can then be
    /// shared into the other vCPUs' VMs via share_smp_pages_from, so all N observe one coherent page.
    pub fn map_smp_shared_fresh_range(&mut self, address: u64, length: u64, permissions: u8) -> bool {
        const PAGE: u64 = 0x1000;
        if address % PAGE != 0 || length == 0 || length % PAGE != 0 {
            return false;
        }
        let native = map_permissions(permissions);
        if std::env::var("SOGEN_ICICLE_SMP_BATCH_MAP").ok().as_deref() == Some("1") {
            let Ok(count) = usize::try_from(length / PAGE) else { return false; };
            return self.vm.cpu.mem.map_smp_shared_fresh_pages(address, count, native);
        }
        let mut page = address;
        let end = address + length;
        while page < end {
            if !self.vm.cpu.mem.map_smp_shared_fresh(page, native) {
                return false;
            }
            page += PAGE;
        }
        true
    }

    /// Map the SAME Arc<PageData> backing as the source VM for each page in the range (SMP), so this
    /// vCPU shares the master's guest RAM. Coherency of the shared bytes is the host CPU's (MESI).
    pub fn share_smp_pages_from(&mut self, source: &IcicleEmulator, address: u64, length: u64) -> bool {
        const PAGE: u64 = 0x1000;
        if address % PAGE != 0 || length == 0 || length % PAGE != 0 {
            return false;
        }
        let mut page = address;
        let end = address + length;
        while page < end {
            let Some(data) = source.vm.cpu.mem.share_page(page) else {
                return false;
            };
            if !self.vm.cpu.mem.map_smp_shared(page, data) {
                return false;
            }
            page += PAGE;
        }
        true
    }

    /// SMP async (step 6.5): capture the shared `Arc<PageData>` for each page of a range on THIS VM's own
    /// thread (safe), so a peer can later alias them WITHOUT reading this (the source) VM cross-thread while
    /// it executes. The returned Arcs are `Send`+`Sync` and carried to peers as an opaque handle.
    pub fn capture_smp_range(&self, address: u64, length: u64) -> Option<Vec<std::sync::Arc<icicle_cpu::mem::physical::PageData>>> {
        const PAGE: u64 = 0x1000;
        if address % PAGE != 0 || length == 0 || length % PAGE != 0 {
            return None;
        }
        let mut pages = Vec::with_capacity((length / PAGE) as usize);
        let mut page = address;
        let end = address + length;
        while page < end {
            pages.push(self.vm.cpu.mem.share_page(page)?);
            page += PAGE;
        }
        Some(pages)
    }

    /// Alias a previously captured range into THIS VM (the peer), on its own thread. No source VM is read.
    pub fn map_captured_smp_range(&mut self, captured: &[std::sync::Arc<icicle_cpu::mem::physical::PageData>], address: u64) -> bool {
        const PAGE: u64 = 0x1000;
        if address % PAGE != 0 {
            return false;
        }
        if std::env::var("SOGEN_ICICLE_SMP_BATCH_MAP").ok().as_deref() == Some("1") {
            return self.vm.cpu.mem.map_smp_shared_pages(address, captured);
        }
        let mut page = address;
        for data in captured {
            if !self.vm.cpu.mem.map_smp_shared(page, data.clone()) {
                return false;
            }
            page += PAGE;
        }
        true
    }

    pub fn map_mmio(
        &mut self,
        address: u64,
        length: u64,
        read_function: Box<dyn Fn(u64, &mut [u8])>,
        write_function: Box<dyn Fn(u64, &[u8])>,
    ) -> bool {
        let mem = self.get_mem();

        let handler = MmioHandler::new(read_function, write_function);
        let handler_id = mem.register_io_handler(handler);

        return self.get_mem().map_memory_len(address, length, handler_id);
    }

    fn reclaim_pending_pages(&mut self) {
        let threshold = (self.vm.cpu.mem.capacity() / 16).clamp(1, 4096);
        if self.vm_running || self.resume_pcode || self.pending_free_pages.is_empty() {
            return;
        }
        if self.pending_free_pages.len() < threshold
            && self.vm.cpu.mem.total_pages().saturating_add(threshold) < self.vm.cpu.mem.capacity()
        {
            return;
        }
        self.flush_pending_code();
        self.vm
            .cpu
            .mem
            .reclaim_unmapped_physical(&self.pending_free_pages, &[]);
        self.pending_free_pages.clear();
    }

    pub fn unmap_memory(&mut self, address: u64, length: u64) -> bool {
        let Some(last) = length
            .checked_sub(1)
            .and_then(|value| address.checked_add(value))
        else {
            return false;
        };
        static UNMAP_ATTRIBUTION: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        static SLOW_UNMAP_REPORTS: std::sync::atomic::AtomicUsize =
            std::sync::atomic::AtomicUsize::new(0);
        let attribution = *UNMAP_ATTRIBUTION
            .get_or_init(|| std::env::var("SOGEN_ICICLE_UNMAP_ATTRIBUTION").as_deref() == Ok("1"));
        let started = attribution.then(Instant::now);
        let pending_before = if attribution {
            self.pending_free_pages.len()
        } else {
            0
        };

        self.invalidate_code_range(address, length, MANUAL_UNMAP);
        let invalidated = attribution.then(Instant::now);
        let mem = &mut self.vm.cpu.mem;
        for (_, _, entry) in mem.mapping.overlapping_iter(address..=last) {
            if let Some(icicle_cpu::mem::MemoryMapping::Physical(page)) = entry {
                if !page.index.is_zero_page() {
                    self.pending_free_pages.push(page.index);
                }
            }
        }
        let collected_pages = if attribution {
            self.pending_free_pages.len().saturating_sub(pending_before)
        } else {
            0
        };
        let collected = attribution.then(Instant::now);
        let result = mem.unmap_memory_len(address, length);
        let unmapped = attribution.then(Instant::now);
        self.reclaim_pending_pages();

        if let (Some(started), Some(invalidated), Some(collected), Some(unmapped)) =
            (started, invalidated, collected, unmapped)
        {
            let ended = Instant::now();
            let total_ms = ended.duration_since(started).as_millis();
            if total_ms >= 1000
                && SLOW_UNMAP_REPORTS.fetch_add(1, std::sync::atomic::Ordering::Relaxed) < 48
            {
                eprintln!(
                    "[ICICLEUNMAP] bytes={} pages={} result={} total_ms={} invalidate_ms={} collect_ms={} mmu_ms={} reclaim_ms={}",
                    length,
                    collected_pages,
                    result,
                    total_ms,
                    invalidated.duration_since(started).as_millis(),
                    collected.duration_since(invalidated).as_millis(),
                    unmapped.duration_since(collected).as_millis(),
                    ended.duration_since(unmapped).as_millis(),
                );
            }
        }
        result
    }

    pub fn protect_memory(&mut self, address: u64, length: u64, permissions: u8) -> bool {
        self.invalidate_code_range(address, length, MANUAL_PROTECT);
        let native_permissions = map_permissions(permissions);
        let res = self
            .get_mem()
            .update_perm(address, length, native_permissions);
        return res.is_ok();
    }

    pub fn write_memory(&mut self, address: u64, data: &[u8]) -> bool {
        let Some(end) = address.checked_add(data.len() as u64) else {
            return false;
        };
        self.invalidate_code_range(address, data.len() as u64, MANUAL_HOST_WRITE);
        let mem = self.get_mem();
        let mut page = mem.page_aligned(address);
        while page < end {
            if let Some(index) = mem.get_physical_index(page) {
                if index.is_zero_page() {
                    // Host writes bypass guest permissions; Icicle's read-only zero page is shared
                    // without COW because permission-checked guest writes cannot modify it.
                    mem.get_physical_mut(index).copy_on_write = true;
                    mem.tlb.remove_write(page);
                }
            }
            let Some(next) = page.checked_add(mem.page_size()) else {
                break;
            };
            page = next;
        }
        let result = mem.write_bytes(address, data, icicle_vm::cpu::mem::perm::NONE);
        mem.observe_external_write(address, data, result);
        match result {
            Ok(()) => true,
            Err(error) => {
                eprintln!(
                    "Icicle memory write failed: address={address:#x} bytes={} error={error:?} physical_pages={} capacity={}",
                    data.len(),
                    mem.total_pages(),
                    mem.capacity()
                );
                // Large host-write failures are rare. Bound the extra map query and log so
                // a bad guest loop cannot flood the capture. Reservations live in the C++
                // memory manager; this records the backing MMU's actual map state.
                static LARGE_WRITE_DIAGNOSTICS: std::sync::atomic::AtomicUsize =
                    std::sync::atomic::AtomicUsize::new(0);
                if data.len() >= 4096
                    && LARGE_WRITE_DIAGNOSTICS.fetch_add(1, std::sync::atomic::Ordering::Relaxed) < 8
                {
                    let last = end - 1;
                    let first_mapping = mem.mapping.get_with_range(address);
                    let last_mapping = mem.mapping.get_with_range(last);
                    let mut previous = None;
                    let mut following = None;
                    for (start, finish, _) in mem.mapping.iter() {
                        if finish < address {
                            previous = Some((start, finish));
                        } else if start > last {
                            following = Some((start, finish));
                            break;
                        }
                    }
                    eprintln!(
                        "[ICWRITE] start_map={first_mapping:?} end_map={last_mapping:?} previous={previous:?} following={following:?}"
                    );
                }
                false
            }
        }
    }

    pub fn read_memory(&mut self, address: u64, data: &mut [u8]) -> bool {
        let res = self
            .get_mem()
            .read_bytes(address, data, icicle_vm::cpu::mem::perm::NONE);
        return res.is_ok();
    }

    pub fn save_registers(&self) -> Vec<u8> {
        const REG_SIZE: usize = std::mem::size_of::<icicle_cpu::Regs>();
        unsafe {
            let data: [u8; REG_SIZE] = self.vm.cpu.regs.read_at(0);
            return data.to_vec();
        }
    }

    pub fn restore_registers(&mut self, data: &[u8]) {
        const REG_SIZE: usize = std::mem::size_of::<icicle_cpu::Regs>();

        let mut buffer: [u8; REG_SIZE] = [0; REG_SIZE];
        let size = std::cmp::min(REG_SIZE, data.len());
        buffer.copy_from_slice(&data[..size]);

        unsafe {
            self.vm.cpu.regs.write_at(0, buffer);
        };
        crate::xstate::restore_legacy_xcr0(&mut self.vm.cpu);
        crate::packed_sad::restore_legacy_controls(&mut self.vm.cpu);
    }

    // Deserializing reuses the live VM, so any execution state that a freshly built VM would not carry must be
    // dropped here. save_registers/restore_registers only round-trip the register space; the instruction counter
    // and the translated (and recompiled) code cache persist otherwise, so a restored run continues on state left
    // over from the previous one instead of matching a fresh VM.
    pub fn reset_volatile_state(&mut self) {
        self.vm.cpu.icount = 0;
        self.vm.code.flush_code();
    }

    fn read_generic_register(&mut self, reg: registers::X86Register, buffer: &mut [u8]) -> usize {
        let reg_node = self.reg.get_node(reg);

        let mut bytes = [0u8; 32];
        if reg_node.size == 32 {
            bytes[..16]
                .copy_from_slice(&self.vm.cpu.read::<[u8; 16]>(reg_node.slice(0, 16).into()));
            bytes[16..]
                .copy_from_slice(&self.vm.cpu.read::<[u8; 16]>(reg_node.slice(16, 16).into()));
        } else if (11..=15).contains(&reg_node.size) {
            bytes[..8].copy_from_slice(&self.vm.cpu.read::<[u8; 8]>(reg_node.slice(0, 8).into()));
            for offset in 8..reg_node.size {
                bytes[usize::from(offset)] = self.vm.cpu.read_var::<u8>(reg_node.slice(offset, 1));
            }
        } else {
            bytes = self.vm.cpu.read_dynamic(pcode::Value::Var(reg_node)).zxt();
        }

        let len = std::cmp::min(bytes.len(), buffer.len());
        buffer[..len].copy_from_slice(&bytes[..len]);

        return reg_node.size.into();
    }

    fn read_flags<T>(&mut self, data: &mut [u8]) -> usize {
        const REAL_SIZE: usize = std::mem::size_of::<u64>();
        let limit: usize = std::mem::size_of::<T>();
        let size = std::cmp::min(REAL_SIZE, limit);

        let flags: u64 = self.reg.get_flags(&mut self.vm.cpu);

        let copy_size = std::cmp::min(data.len(), size);
        data[..copy_size].copy_from_slice(&flags.to_ne_bytes()[..copy_size]);

        return limit;
    }

    pub fn read_register(&mut self, reg: registers::X86Register, data: &mut [u8]) -> usize {
        if let Some(size) = crate::aligned_move::read_register(&self.vm.cpu, &reg, data) {
            return size;
        }
        match reg {
            registers::X86Register::Rflags => self.read_flags::<u64>(data),
            registers::X86Register::Eflags => self.read_flags::<u32>(data),
            registers::X86Register::Flags => self.read_flags::<u16>(data),
            _ => self.read_generic_register(reg, data),
        }
    }

    pub fn create_snapshot(&mut self) -> u32 {
        if self.has_host_mappings() { return u32::MAX; }
        let snap = self.vm.snapshot();

        let id = self.snapshots.len() as u32;
        self.snapshots
            .push((Box::new(snap), self.pending_free_pages.clone()));

        return id;
    }

    pub fn restore_snapshot(&mut self, id: u32) {
        let (snapshot, pending_free_pages) = &self.snapshots[id as usize];
        self.vm.restore(snapshot);
        self.pending_free_pages.clone_from(pending_free_pages);
    }

    fn write_flags<T>(&mut self, data: &[u8]) -> usize {
        const REAL_SIZE: usize = std::mem::size_of::<u64>();
        let limit: usize = std::mem::size_of::<T>();
        let size = std::cmp::min(REAL_SIZE, limit);
        let copy_size = std::cmp::min(data.len(), size);

        let mut buffer = [0u8; REAL_SIZE];
        self.read_flags::<u64>(&mut buffer);

        buffer[..copy_size].copy_from_slice(&data[..copy_size]);

        let flags = u64::from_ne_bytes(buffer);
        self.reg.set_flags(&mut self.vm.cpu, flags);

        return limit;
    }

    pub fn write_register(&mut self, reg: registers::X86Register, data: &[u8]) -> usize {
        if let Some(size) = crate::aligned_move::write_register(&mut self.vm.cpu, &reg, data) {
            return size;
        }
        match reg {
            registers::X86Register::Rflags => self.write_flags::<u64>(data),
            registers::X86Register::Eflags => self.write_flags::<u32>(data),
            registers::X86Register::Flags => self.write_flags::<u16>(data),
            _ => self.write_generic_register(reg, data),
        }
    }

    fn write_generic_register(&mut self, reg: registers::X86Register, data: &[u8]) -> usize {
        let reg_node = self.reg.get_node(reg);

        let mut buffer = [0u8; 32];
        let len = std::cmp::min(data.len(), buffer.len());
        buffer[..len].copy_from_slice(&data[..len]);

        //let value = icicle_cpu::regs::DynamicValue::new(buffer, reg_node.size.into());
        //self.vm.cpu.write_trunc(reg_node, value);

        let cpu = &mut self.vm.cpu;

        match reg_node.size {
            1 => cpu.write_var::<[u8; 1]>(reg_node, buffer[..1].try_into().unwrap()),
            2 => cpu.write_var::<[u8; 2]>(reg_node, buffer[..2].try_into().unwrap()),
            3 => cpu.write_var::<[u8; 3]>(reg_node, buffer[..3].try_into().unwrap()),
            4 => cpu.write_var::<[u8; 4]>(reg_node, buffer[..4].try_into().unwrap()),
            5 => cpu.write_var::<[u8; 5]>(reg_node, buffer[..5].try_into().unwrap()),
            6 => cpu.write_var::<[u8; 6]>(reg_node, buffer[..6].try_into().unwrap()),
            7 => cpu.write_var::<[u8; 7]>(reg_node, buffer[..7].try_into().unwrap()),
            8 => cpu.write_var::<[u8; 8]>(reg_node, buffer[..8].try_into().unwrap()),
            9 => cpu.write_var::<[u8; 9]>(reg_node, buffer[..9].try_into().unwrap()),
            10 => cpu.write_var::<[u8; 10]>(reg_node, buffer[..10].try_into().unwrap()),
            11 => cpu.write_var::<[u8; 11]>(reg_node, buffer[..11].try_into().unwrap()),
            12 => cpu.write_var::<[u8; 12]>(reg_node, buffer[..12].try_into().unwrap()),
            13 => cpu.write_var::<[u8; 13]>(reg_node, buffer[..13].try_into().unwrap()),
            14 => cpu.write_var::<[u8; 14]>(reg_node, buffer[..14].try_into().unwrap()),
            15 => cpu.write_var::<[u8; 15]>(reg_node, buffer[..15].try_into().unwrap()),
            16 => cpu.write_var::<[u8; 16]>(reg_node, buffer[..16].try_into().unwrap()),
            32 => {
                cpu.write_var::<[u8; 16]>(reg_node.slice(0, 16), buffer[..16].try_into().unwrap());
                cpu.write_var::<[u8; 16]>(reg_node.slice(16, 16), buffer[16..].try_into().unwrap());
            }
            _ => panic!("invalid dynamic value size"),
        }

        return reg_node.size.into();
    }
}

#[cfg(test)]
mod page_reclamation_tests {
    use super::*;

    const ADDRESS: u64 = 0x10000;

    fn emulator(capacity: usize) -> IcicleEmulator {
        let mut emu = IcicleEmulator::new();
        assert!(emu.vm.cpu.mem.set_capacity(capacity));
        emu
    }

    fn map_data(emu: &mut IcicleEmulator, address: u64, byte: u8) {
        assert!(emu.map_memory(address, 4096, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));
        assert!(emu.write_memory(address, &[byte; 4096]));
    }

    #[test]
    fn host_unmaps_reuse_pages_and_find_partial_mappings() {
        let mut emu = emulator(16);
        for _ in 0..1000 {
            map_data(&mut emu, ADDRESS, 0x41);
            assert!(emu.unmap_memory(ADDRESS, 2048));
            assert!(emu.unmap_memory(ADDRESS + 2048, 2048));
            assert_eq!(emu.vm.cpu.mem.total_pages(), 2);
        }
    }

    #[test]
    fn split_executed_shared_page_reclaims_after_final_unmap() {
        let mut emu = emulator(16);
        assert!(emu.map_smp_shared_fresh_range(ADDRESS, 4096, FOREIGN_READ | FOREIGN_EXEC));
        assert!(emu.unmap_memory(ADDRESS, 2048));

        let index = emu.vm.cpu.mem.get_physical_index(ADDRESS + 2048).unwrap();
        emu.vm.cpu.mem.get_physical_mut(index).executed = true;

        assert!(emu.unmap_memory(ADDRESS + 2048, 2048));
        assert_eq!(emu.vm.cpu.mem.total_pages(), 2);
    }

    #[test]
    fn reclamation_waits_for_vm_and_pcode_boundaries() {
        let mut emu = emulator(16);
        map_data(&mut emu, ADDRESS, 0x41);
        emu.vm_running = true;
        assert!(emu.unmap_memory(ADDRESS, 4096));
        assert_eq!(emu.vm.cpu.mem.total_pages(), 3);
        emu.vm_running = false;
        emu.resume_pcode = true;
        emu.reclaim_pending_pages();
        assert_eq!(emu.vm.cpu.mem.total_pages(), 3);
        emu.resume_pcode = false;
        emu.reclaim_pending_pages();
        assert_eq!(emu.vm.cpu.mem.total_pages(), 2);
    }

    #[test]
    fn snapshot_restores_pending_candidates_with_physical_state() {
        let mut emu = emulator(32);
        map_data(&mut emu, ADDRESS, 0x41);
        assert!(emu.unmap_memory(ADDRESS, 4096));
        assert_eq!(emu.pending_free_pages.len(), 1);
        let saved = emu.create_snapshot();
        map_data(&mut emu, ADDRESS + 4096, 0x42);
        assert!(emu.unmap_memory(ADDRESS + 4096, 4096));
        assert!(emu.pending_free_pages.is_empty());
        emu.restore_snapshot(saved);
        assert_eq!(emu.pending_free_pages.len(), 1);
        map_data(&mut emu, ADDRESS + 8192, 0x43);
        assert!(emu.unmap_memory(ADDRESS + 8192, 4096));
        assert_eq!(emu.vm.cpu.mem.total_pages(), 2);
    }

    #[test]
    fn remapped_code_executes_new_bytes_after_reclamation() {
        let mut emu = emulator(16);
        for value in 1u32..=20 {
            map_data(&mut emu, ADDRESS, 0x90);
            let mut code = vec![0xb8];
            code.extend_from_slice(&value.to_le_bytes());
            code.extend_from_slice(&[0xeb, 0xfe]);
            assert!(emu.write_memory(ADDRESS, &code));
            emu.write_register(registers::X86Register::Rip, &ADDRESS.to_le_bytes());
            emu.start(1000);
            assert!(!emu.vm.code.blocks.is_empty());
            let mut actual = [0; 8];
            emu.read_register(registers::X86Register::Rax, &mut actual);
            assert_eq!(u64::from_le_bytes(actual), value as u64);
            assert!(emu.unmap_memory(ADDRESS, 4096));
            assert_eq!(emu.vm.cpu.mem.total_pages(), 2);
        }
    }
}

#[cfg(test)]
mod hook_container_tests {
    use super::HookContainer;
    use std::cell::RefCell;
    use std::rc::Rc;

    fn add(
        container: &mut HookContainer<dyn Fn()>,
        observed: &Rc<RefCell<Vec<u32>>>,
        value: u32,
    ) -> u32 {
        let observed = observed.clone();
        container.add_hook(Box::new(move || observed.borrow_mut().push(value)))
    }

    fn observed_values(observed: &Rc<RefCell<Vec<u32>>>) -> Vec<u32> {
        let mut values = std::mem::take(&mut *observed.borrow_mut());
        values.sort_unstable();
        values
    }

    #[test]
    fn removal_preserves_other_hook_ids_and_callbacks() {
        let observed = Rc::new(RefCell::new(Vec::new()));
        let mut container: HookContainer<dyn Fn()> = HookContainer::new();
        let first = add(&mut container, &observed, 10);
        let second = add(&mut container, &observed, 20);
        let third = add(&mut container, &observed, 30);
        container.remove_hook(second);
        container.for_each_hook(|callback| callback());
        assert_eq!(observed_values(&observed), vec![10, 30]);
        container.access_hook(third, |callback| callback());
        container.access_hook(second, |callback| callback());
        container.access_hook(first, |callback| callback());
        assert_eq!(observed_values(&observed), vec![10, 30]);
        container.remove_hook(first);
        container.remove_hook(third);
        assert!(container.is_empty());
        assert!(add(&mut container, &observed, 40) > third);
    }

    #[test]
    fn deferred_changes_wait_for_the_outermost_iteration() {
        let observed = Rc::new(RefCell::new(Vec::new()));
        let mut container: HookContainer<dyn Fn()> = HookContainer::new();
        let first = add(&mut container, &observed, 10);
        let second = add(&mut container, &observed, 20);
        let outer = container.do_pre_access_work();
        container.remove_hook(first);
        let third = add(&mut container, &observed, 30);
        container.for_each_hook(|callback| callback());
        assert_eq!(observed_values(&observed), vec![10, 20]);
        container.access_hook(third, |callback| callback());
        assert!(observed_values(&observed).is_empty());
        container.access_hook(second, |callback| callback());
        assert_eq!(observed_values(&observed), vec![20]);
        container.do_post_access_work(outer);
        container.for_each_hook(|callback| callback());
        assert_eq!(observed_values(&observed), vec![20, 30]);
    }

    #[test]
    fn empty_and_missing_hook_dispatch_are_noops() {
        let mut container: HookContainer<dyn Fn()> = HookContainer::new();
        container.for_each_hook(|_| panic!("unexpected callback"));
        container.access_hook(77, |_| panic!("unexpected callback"));
        container.remove_hook(77);
        assert!(container.is_empty());
    }
}

#[cfg(test)]
mod execution_hook_filter_tests {
    use super::ExecutionHooks;
    use std::cell::{Cell, RefCell};
    use std::rc::Rc;

    fn hooks() -> ExecutionHooks {
        ExecutionHooks::new(Rc::new(RefCell::new(false)), Rc::new(Cell::new(false)))
    }

    #[test]
    fn scheduled_callbacks_precede_instruction_callbacks() {
        let calls = Rc::new(RefCell::new(Vec::new()));
        let mut hooks = hooks();
        for index in 0..2 {
            let calls = calls.clone();
            hooks.schedule(Box::new(move || calls.borrow_mut().push(index)));
        }
        let observed = calls.clone();
        hooks.add_generic_hook(Box::new(move |_| observed.borrow_mut().push(2)));
        hooks.run_hooks(0x1000);
        hooks.run_hooks(0x1001);
        assert_eq!(*calls.borrow(), vec![0, 1, 2, 2]);
        assert!(hooks.one_time_callbacks.is_empty());
    }

    #[test]
    #[ignore]
    fn instruction_dispatch_throughput() {
        let iterations = 20_000_000u64;
        let mut hooks = hooks();
        let total = Rc::new(Cell::new(0u64));
        let addresses = Rc::new(Cell::new(0u64));
        for _ in 0..2 {
            let total = total.clone();
            let addresses = addresses.clone();
            hooks.add_generic_hook(Box::new(move |address| {
                total.set(total.get() + 1);
                addresses.set(addresses.get().wrapping_add(address));
            }));
        }
        let ranged = total.clone();
        hooks.add_range_hook(0x1000, 64, Box::new(move |_| ranged.set(ranged.get() + 1)));
        let exact = total.clone();
        hooks.add_specific_hook(0x1020, Box::new(move |_| exact.set(exact.get() + 1)));
        let mut times = Vec::new();
        for _ in 0..5 {
            total.set(0);
            addresses.set(0);
            let started = std::time::Instant::now();
            for index in 0..iterations {
                std::hint::black_box(&mut hooks).run_hooks(0x1000 + (index & 127));
            }
            times.push(started.elapsed().as_secs_f64());
            assert_eq!(
                total.get(),
                iterations * 2 + iterations / 2 + iterations / 128
            );
            assert_eq!(addresses.get(), iterations * 2 * 0x1000 + iterations * 127);
        }
        eprintln!(
            "instruction_dispatch_seconds={times:?}; iterations={iterations}; callbacks_verified=true"
        );
    }

    #[test]
    fn empty_exact_filter_preserves_generic_and_range_callbacks() {
        let calls = Rc::new(RefCell::new(Vec::new()));
        let mut hooks = hooks();
        let generic = calls.clone();
        hooks.add_generic_hook(Box::new(move |address| {
            generic.borrow_mut().push((0, address))
        }));
        let ranged = calls.clone();
        hooks.add_range_hook(
            0x1000,
            2,
            Box::new(move |address| ranged.borrow_mut().push((1, address))),
        );
        hooks.run_hooks(0x1001);
        hooks.run_hooks(0x1002);
        assert_eq!(*calls.borrow(), vec![(0, 0x1001), (1, 0x1001), (0, 0x1002)]);
    }

    #[test]
    fn collisions_and_shared_addresses_keep_exact_dispatch() {
        let calls = Rc::new(RefCell::new(Vec::new()));
        let mut hooks = hooks();
        let address = 0x140001234;
        let collision = (address + 1..address + 0x10000)
            .find(|&candidate| {
                ExecutionHooks::address_filter_bit(candidate)
                    == ExecutionHooks::address_filter_bit(address)
            })
            .unwrap();
        let first_calls = calls.clone();
        let first =
            hooks.add_specific_hook(address, Box::new(move |_| first_calls.borrow_mut().push(1)));
        let second_calls = calls.clone();
        let second = hooks.add_specific_hook(
            address,
            Box::new(move |_| second_calls.borrow_mut().push(2)),
        );
        hooks.run_hooks(collision);
        assert!(calls.borrow().is_empty());
        let third_calls = calls.clone();
        let third = hooks.add_specific_hook(
            collision,
            Box::new(move |_| third_calls.borrow_mut().push(3)),
        );
        hooks.remove_specific_hook(first);
        hooks.run_hooks(address);
        hooks.run_hooks(collision);
        assert_eq!(*calls.borrow(), vec![2, 3]);
        calls.borrow_mut().clear();
        hooks.remove_specific_hook(second);
        hooks.run_hooks(address);
        hooks.run_hooks(collision);
        assert_eq!(*calls.borrow(), vec![3]);
        hooks.remove_specific_hook(third);
        assert_eq!(hooks.address_filter, [0; 4]);
    }

    #[test]
    fn high_addresses_and_removals_do_not_lose_hooks() {
        let calls = Rc::new(RefCell::new(Vec::new()));
        let mut hooks = hooks();
        let mut ids = Vec::new();
        let mut addresses = vec![0, u64::MAX, 0x1800a0330, 0x7ff700000000];
        for index in 0..128u64 {
            addresses.push(index.wrapping_mul(0x2545f4914f6cdd1d));
        }
        addresses.sort_unstable();
        addresses.dedup();
        for &address in &addresses {
            let observed = calls.clone();
            ids.push(hooks.add_specific_hook(
                address,
                Box::new(move |actual| {
                    assert_eq!(actual, address);
                    observed.borrow_mut().push(actual);
                }),
            ));
        }
        for &address in &addresses {
            hooks.run_hooks(address);
        }
        assert_eq!(*calls.borrow(), addresses);
        calls.borrow_mut().clear();
        for &id in ids.iter().step_by(2) {
            hooks.remove_specific_hook(id);
        }
        for &address in &addresses {
            hooks.run_hooks(address);
        }
        assert_eq!(
            *calls.borrow(),
            addresses.into_iter().skip(1).step_by(2).collect::<Vec<_>>()
        );
    }
}

#[cfg(test)]
mod vm_exit_tests {
    use super::*;

    #[test]
    fn guest_store_reports_memory_exhaustion_and_can_resume() {
        let mut emu = IcicleEmulator::new();
        assert!(emu.vm.cpu.mem.set_capacity(3));
        assert!(emu.map_memory(0x10000, 4096, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));
        assert!(emu.write_memory(0x10000, &[0x89, 0x00, 0xeb, 0xfe]));
        assert!(emu.map_memory(0x20000, 4096, FOREIGN_READ | FOREIGN_WRITE));
        emu.vm.cpu.write_pc(0x10000);
        emu.write_register(registers::X86Register::Rax, &0x20000u64.to_le_bytes());
        emu.start(1);
        let stop = emu.last_stop_info();
        assert_eq!(stop.kind, IcicleStopInfo::OTHER);
        assert_eq!(stop.code, ExceptionCode::OutOfMemory as u32);
        assert_eq!(stop.value, 0x20000);
        assert!(
            emu.vm_exit_description()
                .starts_with("OutOfMemory; physical_pages=3/3;")
        );
        assert_eq!(emu.vm.cpu.read_pc(), 0x10000);
        assert!(emu.vm.cpu.mem.set_capacity(4));
        emu.start(1);
        assert_eq!(emu.last_stop_info().kind, IcicleStopInfo::INSTRUCTION_LIMIT);
        let mut value = [0; 4];
        assert!(emu.read_memory(0x20000, &mut value));
        assert_eq!(u32::from_le_bytes(value), 0x20000);
    }
}

#[cfg(test)]
mod memory_limit_tests {
    use super::*;

    #[test]
    fn validates_limits_without_changing_the_previous_capacity_on_error() {
        let mut emu = IcicleEmulator::new();
        assert!(emu.set_memory_limit_mib(1));
        assert_eq!(emu.vm.cpu.mem.capacity(), 256);
        for invalid in [0, u64::MAX, 16_777_216] {
            assert!(!emu.set_memory_limit_mib(invalid));
            assert_eq!(emu.vm.cpu.mem.capacity(), 256);
        }
        assert!(emu.set_memory_limit_mib(4096));
        assert_eq!(emu.vm.cpu.mem.capacity(), 1_048_576);
        assert_eq!(emu.vm.cpu.mem.total_pages(), 2);
    }
}

#[cfg(test)]
mod aligned_move_decode_tests {
    use super::*;
    #[test]
    fn instruction_decoder_preserves_length_and_old_register_layout() {
        let mut emu = IcicleEmulator::new();
        for line in include_str!("legacy-register-layout.txt").lines() {
            let parts: Vec<_> = line.split_whitespace().collect();
            let old_id: i16 = parts[1].parse().unwrap();
            let old_offset: u8 = parts[2].parse().unwrap();
            let old_size: u8 = parts[3].parse().unwrap();
            let current = emu
                .vm
                .cpu
                .arch
                .sleigh
                .get_reg(parts[0])
                .unwrap()
                .get_raw_var();
            assert_eq!(
                current,
                pcode::VarNode::new(old_id, old_offset + old_size).slice(old_offset, old_size),
                "{}",
                parts[0]
            );
        }
        assert!(emu.map_memory(0x10000, 4096, 7));
        for bytes in [
            &[0x66, 0x0f, 0x6f, 0xca][..],
            &[0xc5, 0xf9, 0x6f, 0xca][..],
            &[0x62, 0xf1, 0x7d, 0x48, 0x6f, 0xca][..],
        ] {
            assert!(emu.write_memory(0x10000, bytes));
            emu.vm.cpu.write_pc(0x10000);
            let mut lifter = icicle_cpu::lifter::InstructionLifter::new();
            lifter.set_context(emu.vm.cpu.arch.isa_mode_context[0]);
            let next = lifter
                .lift(&mut *emu.vm.cpu, 0x10000)
                .unwrap_or_else(|e| panic!("{bytes:x?}: {e:?}"));
            assert_eq!(next, 0x10000 + bytes.len() as u64, "{}", lifter.disasm);
        }
    }
}

#[cfg(test)]
mod movemask_decode_tests {
    use super::*;
    #[test]
    fn move_masks_decode() {
        let mut emu = IcicleEmulator::new();
        assert!(emu.map_memory(0x10000, 4096, 7));
        for bytes in [
            &[0x0f, 0x50, 0xc4][..],
            &[0x66, 0x0f, 0x50, 0xc4][..],
            &[0xc5, 0xf8, 0x50, 0xc4][..],
        ] {
            assert!(emu.write_memory(0x10000, bytes));
            emu.vm.cpu.write_pc(0x10000);
            let mut lifter = icicle_cpu::lifter::InstructionLifter::new();
            lifter.set_context(emu.vm.cpu.arch.isa_mode_context[0]);
            let next = lifter
                .lift(&mut *emu.vm.cpu, 0x10000)
                .unwrap_or_else(|e| panic!("{bytes:x?}: {e:?}"));
            assert_eq!(next, 0x10000 + bytes.len() as u64, "{}", lifter.disasm);
        }
    }
}

#[cfg(test)]
mod hook_hotpath_tests {
    use super::*;

    #[test]
    fn single_and_multiple_dispatch_preserve_deferred_changes_and_order() {
        for count in [1, 3] {
            let seen = Rc::new(RefCell::new(Vec::new()));
            let mut hooks: HookContainer<dyn Fn()> = HookContainer::new();
            let mut ids = Vec::new();
            for index in 0..count {
                let seen = seen.clone();
                ids.push(hooks.add_hook(Box::new(move || seen.borrow_mut().push(index))));
            }
            let outer = hooks.do_pre_access_work();
            hooks.remove_hook(ids[0]);
            let added_seen = seen.clone();
            let added = hooks.add_hook(Box::new(move || added_seen.borrow_mut().push(99)));
            hooks.for_each_hook(|callback| callback());
            assert_eq!(*seen.borrow(), (0..count).collect::<Vec<_>>());
            assert!(hooks.is_iterating);
            assert_eq!(hooks.hooks_to_remove.len(), 1);
            assert_eq!(hooks.hooks_to_add.len(), 1);
            hooks.access_hook(added, |_| panic!("deferred addition visible early"));
            hooks.do_post_access_work(outer);
            assert!(!hooks.is_iterating);
            assert!(hooks.hooks_to_remove.is_empty());
            assert!(hooks.hooks_to_add.is_empty());
            seen.borrow_mut().clear();
            hooks.for_each_hook(|callback| callback());
            let mut expected: Vec<_> = (1..count).collect();
            expected.push(99);
            assert_eq!(*seen.borrow(), expected);
        }
    }

    #[test]
    fn scheduled_generic_range_and_exact_order_survives_cardinality_changes() {
        for generic_count in [0, 1, 3] {
            let seen = Rc::new(RefCell::new(Vec::new()));
            let mut hooks =
                ExecutionHooks::new(Rc::new(RefCell::new(false)), Rc::new(Cell::new(false)));
            let address = 0xffff_0123_4567_89abu64;
            let scheduled = seen.clone();
            hooks.schedule(Box::new(move || scheduled.borrow_mut().push((0, 0))));
            let mut ids = Vec::new();
            for index in 0..generic_count {
                let seen = seen.clone();
                ids.push(hooks.add_generic_hook(Box::new(move |actual| {
                    seen.borrow_mut().push((10 + index, actual));
                })));
            }
            for index in 0..2 {
                let seen = seen.clone();
                hooks.add_range_hook(
                    address,
                    2,
                    Box::new(move |actual| {
                        seen.borrow_mut().push((20 + index, actual));
                    }),
                );
            }
            for index in 0..2 {
                let seen = seen.clone();
                hooks.add_specific_hook(
                    address,
                    Box::new(move |actual| {
                        seen.borrow_mut().push((30 + index, actual));
                    }),
                );
            }
            hooks.run_hooks(address);
            let mut expected = vec![(0, 0)];
            expected.extend((0..generic_count).map(|index| (10 + index, address)));
            expected.extend([(20, address), (21, address), (30, address), (31, address)]);
            assert_eq!(*seen.borrow(), expected);
            for id in ids {
                hooks.remove_generic_hook(id);
            }
            seen.borrow_mut().clear();
            hooks.run_hooks(address + 1);
            assert_eq!(*seen.borrow(), vec![(20, address + 1), (21, address + 1)]);
        }
    }

    #[test]
    fn block_callbacks_keep_full_address_and_instruction_count() {
        for count in [0, 1, 3] {
            let seen = Rc::new(RefCell::new(Vec::new()));
            let mut hooks =
                ExecutionHooks::new(Rc::new(RefCell::new(false)), Rc::new(Cell::new(false)));
            for index in 0..count {
                let seen = seen.clone();
                hooks.add_block_hook(Box::new(move |address, instructions| {
                    seen.borrow_mut().push((index, address, instructions));
                }));
            }
            hooks.on_block(0xffff_1234_5678_9abc, 0x1_0000_0001);
            assert_eq!(
                *seen.borrow(),
                (0..count)
                    .map(|index| (index, 0xffff_1234_5678_9abc, 0x1_0000_0001))
                    .collect::<Vec<_>>()
            );
        }
    }

    #[test]
    fn invalidation_before_dispatch_suppresses_every_callback() {
        let mut vm = create_x64_vm();
        let mut hooks = ExecutionHooks::new(Rc::new(RefCell::new(true)), Rc::new(Cell::new(true)));
        hooks.schedule(Box::new(|| panic!("scheduled callback after invalidation")));
        hooks.add_generic_hook(Box::new(|_| panic!("generic callback after invalidation")));
        hooks.execute(&mut vm.cpu, 0x1_0000_0000);
        assert_eq!(vm.cpu.exception.code, ExceptionCode::Environment as u32);
        assert_eq!(vm.cpu.exception.value, CACHE_INVALIDATED);
        assert_eq!(hooks.one_time_callbacks.len(), 1);
    }

    #[test]
    fn callback_stop_keeps_remaining_callbacks_and_precedes_invalidation() {
        for stop_requested in [false, true] {
            let mut vm = create_x64_vm();
            let stop = Rc::new(RefCell::new(false));
            let invalidate = Rc::new(Cell::new(false));
            let seen = Rc::new(RefCell::new(Vec::new()));
            let mut hooks = ExecutionHooks::new(stop.clone(), invalidate.clone());
            let address = 0x1234_5678_9abcu64;
            let requested = seen.clone();
            hooks.add_generic_hook(Box::new(move |actual| {
                requested.borrow_mut().push((0, actual));
                *stop.borrow_mut() = stop_requested;
                invalidate.set(true);
            }));
            let generic = seen.clone();
            hooks.add_generic_hook(Box::new(move |actual| {
                generic.borrow_mut().push((1, actual))
            }));
            let ranged = seen.clone();
            hooks.add_range_hook(
                address,
                2,
                Box::new(move |actual| ranged.borrow_mut().push((2, actual))),
            );
            let exact = seen.clone();
            hooks.add_specific_hook(
                address,
                Box::new(move |actual| exact.borrow_mut().push((3, actual))),
            );
            hooks.execute(&mut vm.cpu, address);
            assert_eq!(
                *seen.borrow(),
                vec![(0, address), (1, address), (2, address), (3, address)]
            );
            if stop_requested {
                assert_eq!(
                    vm.cpu.exception.code,
                    ExceptionCode::InstructionLimit as u32
                );
                assert_eq!(vm.cpu.exception.value, address);
            } else {
                assert_eq!(vm.cpu.exception.code, ExceptionCode::Environment as u32);
                assert_eq!(vm.cpu.exception.value, CACHE_INVALIDATED);
            }
        }
    }
}

#[cfg(test)]
mod lean_exact_execution_hook_tests {
    use super::*;

    const ADDRESS: u64 = 0x9a000;

    fn vm(probe: bool) -> IcicleEmulator {
        let mut emu = IcicleEmulator::new_with_hook_modes(false, probe);
        emu.vm.enable_jit = true;
        assert!(emu.map_memory(ADDRESS, 0x1000, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));
        assert!(emu.write_memory(ADDRESS, &[0x90, 0x90, 0xeb, 0xfe]));
        emu
    }

    #[test]
    fn lean_probe_delivers_late_exact_hook_without_full_instrumentation() {
        let mut emu = vm(true);
        // Compile the block before registration: DXVK hooks are installed at module load.
        emu.vm.cpu.write_pc(ADDRESS);
        emu.start(2);
        let seen = Rc::new(RefCell::new(Vec::new()));
        let observed = Rc::clone(&seen);
        let exact = emu.add_execution_hook(
            ADDRESS + 1,
            Box::new(move |addr| observed.borrow_mut().push(addr)),
        );
        let generic_calls = Rc::new(Cell::new(0));
        let generic_observed = Rc::clone(&generic_calls);
        let generic = emu.add_generic_execution_hook(Box::new(move |_| {
            generic_observed.set(generic_observed.get() + 1);
        }));
        emu.vm.cpu.write_pc(ADDRESS);
        emu.start(2);
        assert_eq!(*seen.borrow(), vec![ADDRESS + 1]);
        assert_eq!(generic_calls.get(), 0, "lean probe enabled broad instrumentation");

        emu.remove_hook(exact);
        emu.vm.cpu.write_pc(ADDRESS);
        emu.start(2);
        assert_eq!(*seen.borrow(), vec![ADDRESS + 1], "removed exact hook still ran");
        emu.remove_hook(generic);
    }

    #[test]
    fn probe_off_preserves_lean_noop_barrier_behavior() {
        let mut emu = vm(false);
        let called = Rc::new(Cell::new(false));
        let observed = Rc::clone(&called);
        emu.add_execution_hook(ADDRESS + 1, Box::new(move |_| observed.set(true)));
        emu.vm.cpu.write_pc(ADDRESS);
        emu.start(2);
        assert!(!called.get(), "probe-off lean mode unexpectedly delivered exact hooks");
    }
}

#[cfg(test)]
mod fast_noop_barrier_tests {
    use super::*;

    struct InvalidRegisterBeforeNextMarker {
        address: u64,
        inserted: Rc<Cell<bool>>,
    }

    impl icicle_vm::CodeInjector for InvalidRegisterBeforeNextMarker {
        fn inject(&mut self, _cpu: &mut icicle_vm::cpu::Cpu,
            group: &icicle_vm::cpu::BlockGroup, code: &mut icicle_vm::BlockTable) {
            if group.start != self.address || self.inserted.get() {
                return;
            }
            for id in group.range() {
                let instructions = &mut code.blocks[id].pcode.instructions;
                let Some(second_marker) = instructions.iter().enumerate()
                    .filter(|(_, stmt)| matches!(stmt.op, pcode::Op::InstructionMarker))
                    .nth(1).map(|(index, _)| index) else { continue };
                assert!(matches!(instructions[second_marker + 1].op, pcode::Op::Hook(_)));
                // Interpreter fallback sets UnmappedRegister here. The next lean barrier must
                // observe it before the second guest instruction changes RAX.
                instructions.insert(second_marker, pcode::Instruction {
                    op: pcode::Op::Store(pcode::REGISTER_SPACE),
                    inputs: pcode::Inputs::new(
                        pcode::Value::Const(0xffff_fffe, 8),
                        pcode::Value::Const(0x5a, 1),
                    ),
                    output: pcode::VarNode::NONE,
                });
                self.inserted.set(true);
                return;
            }
        }
    }

    #[test]
    fn fast_noop_barrier_preserves_prior_dynamic_register_exception() {
        if std::env::var("SOGEN_ICICLE_FAST_NOOP_BARRIER").as_deref() != Ok("1")
            || std::env::var("SOGEN_ICICLE_LEAN_BARRIER").as_deref() == Ok("0") {
            return;
        }
        const ADDRESS: u64 = 0x8f000;
        let mut emu = IcicleEmulator::new_with_instruction_hooks(false);
        emu.vm.enable_jit = true;
        assert!(emu.map_memory(ADDRESS, 0x1000, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));
        assert!(emu.write_memory(ADDRESS, &[0x90, 0xb8, 0x78, 0x56, 0x34, 0x12, 0xeb, 0xfe]));
        let inserted = Rc::new(Cell::new(false));
        emu.vm.add_injector(InvalidRegisterBeforeNextMarker {
            address: ADDRESS, inserted: Rc::clone(&inserted),
        });
        emu.vm.cpu.write_pc(ADDRESS);
        emu.start(8);
        assert!(inserted.get());
        assert!(matches!(emu.last_vm_exit,
            icicle_vm::VmExit::UnhandledException((ExceptionCode::UnmappedRegister, 0xffff_fffe))));
        let mut rax = [0u8; 8];
        emu.read_register(registers::X86Register::Rax, &mut rax);
        assert_eq!(u64::from_le_bytes(rax), 0, "second instruction ran after the exception");
    }
}

#[cfg(test)]
mod shared_memory_smp {
    //! P1 foundation for multi-vCPU SMP: can N Icicle VMs share ONE guest address space coherently?
    //! Maps the same host buffer into two VMs and checks a write through one is visible via the
    //! other. Host-memory backing (host hardware keeps it coherent) is a candidate shared-RAM path
    //! for per-vCPU VMs that avoids rewriting PhysicalMemory. Runs under plain `cargo test`.
    use super::*;

    struct RewriteAfterLift {
        address: u64,
        write_address: u64,
        cross_page_marker: Option<u64>,
        writer: Rc<RefCell<IcicleEmulator>>,
        fired: Rc<Cell<bool>>,
    }

    impl icicle_vm::CodeInjector for RewriteAfterLift {
        fn inject(
            &mut self,
            _cpu: &mut icicle_vm::cpu::Cpu,
            group: &icicle_vm::cpu::BlockGroup,
            code: &mut icicle_vm::BlockTable,
        ) {
            if group.start == self.address && !self.fired.replace(true) {
                if let Some(other_page) = self.cross_page_marker {
                    assert!(code.blocks[group.range()].iter().any(|block| {
                        let markers: Vec<_> = block
                            .pcode
                            .instructions
                            .iter()
                            .filter(|stmt| matches!(stmt.op, pcode::Op::InstructionMarker))
                            .map(|stmt| stmt.inputs.first().as_u64())
                            .collect();
                        markers.contains(&self.address) && markers.contains(&other_page)
                    }));
                }
                assert!(self.writer.borrow_mut().write_memory(
                    self.write_address,
                    &[0xB8, 0x22, 0x22, 0x00, 0x00, 0xEB, 0xF9],
                ));
            }
        }
    }

    #[test]
    fn peer_write_after_first_lift_is_seen_with_full_and_lean_hooks() {
        if !smp_prelift_epoch_enabled() || !smp_lean_epoch_hook_enabled() {
            return;
        }
        use icicle_vm::cpu::mem::perm;

        const ADDRESS: u64 = 0x40000;
        for instruction_hooks in [true, false] {
            let writer = Rc::new(RefCell::new(IcicleEmulator::new()));
            let mut reader = IcicleEmulator::new_with_instruction_hooks(instruction_hooks);
            assert!(writer.borrow_mut().vm.cpu.mem.map_smp_shared_fresh(
                ADDRESS,
                perm::READ | perm::WRITE | perm::EXEC,
            ));
            assert!(writer.borrow_mut().write_memory(
                ADDRESS,
                &[0xB8, 0x11, 0x11, 0x00, 0x00, 0xEB, 0xF9],
            ));
            let shared = writer.borrow().vm.cpu.mem.share_page(ADDRESS).unwrap();
            assert!(reader.vm.cpu.mem.map_smp_shared(ADDRESS, shared.clone()));

            let fired = Rc::new(Cell::new(false));
            reader.vm.add_injector(RewriteAfterLift {
                address: ADDRESS,
                write_address: ADDRESS,
                cross_page_marker: None,
                writer: Rc::clone(&writer),
                fired: Rc::clone(&fired),
            });
            reader.vm.cpu.write_pc(ADDRESS);
            reader.start(10);

            let mut result = [0u8; 8];
            reader.read_register(registers::X86Register::Rax, &mut result);
            let index = reader.vm.cpu.mem.get_physical_index(ADDRESS).unwrap();
            assert!(fired.get());
            assert!(shared.code_epoch() > reader.vm.cpu.mem.get_physical(index).smp_mapping_epoch);
            assert_eq!(u64::from_le_bytes(result), 0x2222, "instruction_hooks={instruction_hooks}");
        }
    }

    #[test]
    #[ignore = "expected failure until default lean handles peer writes to translated code"]
    fn default_lean_peer_write_after_lift_needs_invalidation() {
        use icicle_vm::cpu::mem::perm;

        assert!(!smp_lean_epoch_hook_enabled());
        const ADDRESS: u64 = 0x48000;
        let writer = Rc::new(RefCell::new(IcicleEmulator::new()));
        let mut reader = IcicleEmulator::new_with_instruction_hooks(false);
        assert!(writer.borrow_mut().vm.cpu.mem.map_smp_shared_fresh(
            ADDRESS,
            perm::READ | perm::WRITE | perm::EXEC,
        ));
        assert!(writer.borrow_mut().write_memory(
            ADDRESS,
            &[0xB8, 0x11, 0x11, 0x00, 0x00, 0xEB, 0xF9],
        ));
        let shared = writer.borrow().vm.cpu.mem.share_page(ADDRESS).unwrap();
        assert!(reader.vm.cpu.mem.map_smp_shared(ADDRESS, shared.clone()));

        let fired = Rc::new(Cell::new(false));
        reader.vm.add_injector(RewriteAfterLift {
            address: ADDRESS,
            write_address: ADDRESS,
            cross_page_marker: None,
            writer: Rc::clone(&writer),
            fired: Rc::clone(&fired),
        });
        reader.vm.cpu.write_pc(ADDRESS);
        reader.start(10);

        let mut result = [0u8; 8];
        reader.read_register(registers::X86Register::Rax, &mut result);
        assert!(fired.get());
        assert!(shared.code_epoch() > 0);
        assert_eq!(u64::from_le_bytes(result), 0x2222);
    }

    #[test]
    fn peer_write_to_later_page_after_lift_is_seen_by_lean_hooks() {
        if !smp_prelift_epoch_enabled() || !smp_lean_epoch_hook_enabled() {
            return;
        }
        use icicle_vm::cpu::mem::perm;

        const FIRST_PAGE: u64 = 0x60000;
        const SECOND_PAGE: u64 = FIRST_PAGE + 0x1000;
        const START: u64 = SECOND_PAGE - 2;
        let writer = Rc::new(RefCell::new(IcicleEmulator::new()));
        let mut reader = IcicleEmulator::new_with_instruction_hooks(false);
        for page in [FIRST_PAGE, SECOND_PAGE] {
            assert!(writer.borrow_mut().vm.cpu.mem.map_smp_shared_fresh(
                page,
                perm::READ | perm::WRITE | perm::EXEC,
            ));
        }
        assert!(writer.borrow_mut().write_memory(START, &[0x90, 0x90]));
        assert!(writer.borrow_mut().write_memory(
            SECOND_PAGE,
            &[0xB8, 0x11, 0x11, 0x00, 0x00, 0xEB, 0xF9],
        ));
        for page in [FIRST_PAGE, SECOND_PAGE] {
            let shared = writer.borrow().vm.cpu.mem.share_page(page).unwrap();
            assert!(reader.vm.cpu.mem.map_smp_shared(page, shared));
        }

        let fired = Rc::new(Cell::new(false));
        reader.vm.add_injector(RewriteAfterLift {
            address: START,
            write_address: SECOND_PAGE,
            cross_page_marker: Some(SECOND_PAGE),
            writer: Rc::clone(&writer),
            fired: Rc::clone(&fired),
        });
        reader.vm.cpu.write_pc(START);
        reader.start(10);

        let mut result = [0u8; 8];
        reader.read_register(registers::X86Register::Rax, &mut result);
        assert!(fired.get());
        assert_eq!(u64::from_le_bytes(result), 0x2222);
    }

    #[test]
    fn default_first_use_ignores_loader_writes_but_checks_warm_page() {
        if smp_prelift_epoch_enabled() {
            return;
        }
        use icicle_vm::cpu::mem::perm;

        const ADDRESS: u64 = 0x70000;
        let mut vm = create_x64_vm();
        assert!(vm.cpu.mem.map_smp_shared_fresh(
            ADDRESS,
            perm::READ | perm::WRITE | perm::EXEC,
        ));
        vm.cpu.mem.write_bytes(ADDRESS, &[0x90; 256], perm::NONE).unwrap();
        let invalidated = Rc::new(Cell::new(false));
        let mut hooks = ExecutionHooks::new(Rc::new(RefCell::new(false)), invalidated.clone());

        hooks.execute(&mut vm.cpu, ADDRESS);
        assert!(!invalidated.get());

        vm.cpu.mem.write(ADDRESS, [0xCC], perm::NONE).unwrap();
        hooks.execute(&mut vm.cpu, ADDRESS);
        assert!(invalidated.get());
        assert_eq!(vm.cpu.exception.value, CACHE_INVALIDATED);
    }

    #[test]
    fn remapped_shared_page_changes_epoch_identity() {
        use icicle_vm::cpu::mem::perm;

        const ADDRESS: u64 = 0x50000;
        let mut vm = create_x64_vm();
        let invalidated = Rc::new(Cell::new(false));
        let mut hooks = ExecutionHooks::new(Rc::new(RefCell::new(false)), invalidated.clone());
        assert!(vm.cpu.mem.map_smp_shared_fresh(ADDRESS, perm::READ | perm::EXEC));
        hooks.execute(&mut vm.cpu, ADDRESS);
        assert!(!invalidated.get());

        assert!(vm.cpu.mem.unmap_memory_len(ADDRESS, 0x1000));
        assert!(vm.cpu.mem.map_smp_shared_fresh(ADDRESS, perm::READ | perm::EXEC));
        hooks.execute(&mut vm.cpu, ADDRESS);
        assert!(invalidated.get());
        assert_eq!(vm.cpu.exception.code, ExceptionCode::Environment as u32);
        assert_eq!(vm.cpu.exception.value, CACHE_INVALIDATED);
    }

    #[test]
    fn two_vms_share_one_host_backed_region() {
        use std::alloc::{alloc_zeroed, dealloc, Layout};
        // map_host_memory requires a PAGE-ALIGNED host pointer; a Vec<u8> is not aligned.
        let layout = Layout::from_size_align(0x1000, 0x1000).unwrap();
        let host = unsafe { alloc_zeroed(layout) };
        assert!(!host.is_null());
        {
            let mut a = IcicleEmulator::new();
            let mut b = IcicleEmulator::new();
            unsafe {
                assert!(a.map_host_memory(0x20000, host, 0x1000, FOREIGN_READ | FOREIGN_WRITE));
                assert!(b.map_host_memory(0x20000, host, 0x1000, FOREIGN_READ | FOREIGN_WRITE));
            }
            // Write through A -> visible through B (same host bytes).
            assert!(a.write_memory(0x20000, &0xdeadbeefu32.to_le_bytes()));
            let mut buf = [0u8; 4];
            assert!(b.read_memory(0x20000, &mut buf));
            assert_eq!(u32::from_le_bytes(buf), 0xdeadbeef, "A's write must be visible via B");
            // And the reverse.
            assert!(b.write_memory(0x20004, &0x12345678u32.to_le_bytes()));
            let mut buf2 = [0u8; 4];
            assert!(a.read_memory(0x20004, &mut buf2));
            assert_eq!(u32::from_le_bytes(buf2), 0x12345678, "B's write must be visible via A");
            // Direct host mutation is seen by a VM (models a third writer / DMA).
            unsafe { std::ptr::copy_nonoverlapping(0xcafef00du32.to_le_bytes().as_ptr(), host.add(8), 4) };
            let mut buf3 = [0u8; 4];
            assert!(a.read_memory(0x20008, &mut buf3));
            assert_eq!(u32::from_le_bytes(buf3), 0xcafef00d, "host write must be visible via A");
        } // VMs dropped here, before the backing is freed
        unsafe { dealloc(host, layout) };
    }

    /// The C++ `PeerExecutesRegionMappedFromInsideHook` failure reduced to Rust: after a peer VM has
    /// EXECUTED code from an SMP-shared page (page is executed / JIT-translated), a HOST write via the
    /// other VM (bridge write_memory: invalidate_code_range + write_bytes) must be visible to the
    /// first VM's GUEST reads (executed loads), not only to its host reads.
    /// 6.6a A/B experiment (run with SOGEN_SMP_EPOCH=2): the SAME epoch raise/recovery on a
    /// PRIVATE page at N=1. If this recovers (rax=0x2222), the shared-page lift diverges; if it
    /// faults with ReadUnmapped(0) too, the raise itself is malformed vs the working path.
    #[test]
    #[ignore]
    fn epoch_recovery_private_page_ab() {
        let mut a = IcicleEmulator::new();
        assert!(a.map_memory(0x10000, 4096, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));
        let v1: [u8; 7] = [0xB8, 0x11, 0x11, 0x00, 0x00, 0xEB, 0xF9];
        let v2: [u8; 7] = [0xB8, 0x22, 0x22, 0x00, 0x00, 0xEB, 0xF9];
        assert!(a.write_memory(0x10000, &v1));
        a.vm.cpu.write_pc(0x10000);
        a.start(10);
        let mut buf = [0u8; 8];
        a.read_register(registers::X86Register::Rax, &mut buf);
        eprintln!("AB STEP1 rax={:#x} (expect 0x1111)", u64::from_le_bytes(buf));
        assert_eq!(u64::from_le_bytes(buf), 0x1111);

        eprintln!("AB STEP2 host-write v2");
        assert!(a.write_memory(0x10000, &v2));
        // Bump the epoch EXPLICITLY: the host write goes through the cached TLB write pointer, so
        // write_physical (where the mode-2 bump lives) never runs. This forces the mismatch so the
        // RAISE -> flush -> re-fetch path itself is what this A/B exercises.
        {
            let mem = &mut a.vm.cpu.mem;
            let page_start = mem.page_aligned(0x10000);
            let index = mem.get_physical_index(page_start).expect("page");
            mem.get_physical_mut(index).data_mut().bump_code_epoch();
            eprintln!("AB STEP2b epoch bumped manually");
        }

        a.vm.cpu.write_pc(0x10000);
        a.start(10);
        let mut buf2 = [0u8; 8];
        a.read_register(registers::X86Register::Rax, &mut buf2);
        eprintln!("AB STEP3 rax={:#x} (expect 0x2222)", u64::from_le_bytes(buf2));
        assert_eq!(u64::from_le_bytes(buf2), 0x2222, "private-page epoch recovery failed");
    }

    #[test]
    fn guest_read_sees_peer_host_write_to_executed_shared_page() {
        use icicle_vm::cpu::mem::perm;
        let mut a = IcicleEmulator::new();
        let mut b = IcicleEmulator::new();
        let addr = 0x40000u64;
        assert!(a.vm.cpu.mem.map_smp_shared_fresh(addr, perm::READ | perm::WRITE | perm::EXEC));
        let shared = a.vm.cpu.mem.share_page(addr).expect("shared arc");
        assert!(b.vm.cpu.mem.map_smp_shared(addr, shared));

        // Code and TARGET (+0x100) share the executable page. The backward jump keeps
        // execution in known bytes when the instruction budget advances past the load.
        let code: [u8; 12] = [
            0x48, 0xA1, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rax, [0x40100]
            0xEB, 0xF4, // jmp back to the load
        ];
        assert!(a.write_memory(addr, &code));
        assert!(a.write_memory(addr + 0x100, &0x1122334455667788u64.to_le_bytes()));
        b.vm.cpu.write_pc(addr);

        let read_rax = |emu: &mut IcicleEmulator| -> u64 {
            let mut buf = [0u8; 8];
            emu.read_register(registers::X86Register::Rax, &mut buf);
            u64::from_le_bytes(buf)
        };

        // A nonzero first value proves the guest load actually ran. One instruction
        // credit stops at Icicle's marker before executing the load body.
        b.write_register(registers::X86Register::Rax, &0u64.to_le_bytes());
        b.start(2);
        assert_eq!(read_rax(&mut b), 0x1122334455667788, "initial load must execute");

        // 2) A host-writes TARGET (the failing path: invalidate_code_range + write_bytes on VM A).
        assert!(a.write_memory(addr + 0x100, &0xfeedfacefeedfaceu64.to_le_bytes()));

        // 3) Host-read sanity through BOTH VMs.
        let mut buf = [0u8; 8];
        assert!(a.read_memory(addr + 0x100, &mut buf));
        assert_eq!(u64::from_le_bytes(buf), 0xfeedfacefeedface, "A host-read");
        assert!(b.read_memory(addr + 0x100, &mut buf));
        assert_eq!(u64::from_le_bytes(buf), 0xfeedfacefeedface, "B host-read (shared bytes)");

        // Repeat the guest load from the already translated page after the peer write.
        b.vm.cpu.write_pc(addr);
        b.start(2);
        assert_eq!(read_rax(&mut b), 0xfeedfacefeedface, "B's guest load must see A's write");
    }

    /// Concurrency stage of the C++ `PeerExecutesRegionMappedFromInsideHook` reduction: B's JITTED
    /// poll loop runs on its own thread over the shared page while A host-writes the cell mid-flight.
    /// (The sequential variant passes; if this passes too, the trigger is the hook context or the
    /// kick/drain path, not raw concurrency.)
    /// 6.6b: a PROTECT on an SMP-shared page must not privatize it. Mmu::protect used
    /// Page::data_mut() (Arc::make_mut), which clones a shared page in the protecting VM — after
    /// which that VM's writes/perm changes diverge from the rest (the probe's WritePerm fault on
    /// ntdll .data at pc=0x180075d6a traces to this). Sharing must survive protects.
    #[test]
    fn protect_does_not_privatize_shared_page() {
        use icicle_vm::cpu::mem::perm;
        const PAGE: u64 = 0x40000;
        let mut a = IcicleEmulator::new();
        let mut b = IcicleEmulator::new();
        assert!(a.vm.cpu.mem.map_smp_shared_fresh(PAGE, perm::READ | perm::WRITE));
        let shared = a.vm.cpu.mem.share_page(PAGE).expect("arc");
        assert!(b.vm.cpu.mem.map_smp_shared(PAGE, shared));

        // B writes; A protects RW (the loader's everyday pattern); B writes again.
        assert!(b.write_memory(PAGE, &1u64.to_le_bytes()));
        assert!(a.vm.cpu.mem.update_perm(PAGE, 0x1000, perm::READ | perm::WRITE).is_ok());
        assert!(b.write_memory(PAGE + 8, &2u64.to_le_bytes()));

        // Both VMs must still see BOTH writes (one shared backing), and B's page must still be
        // smp_shared (not a private clone).
        let check = |emu: &mut IcicleEmulator, tag: &str| {
            let mut buf = [0u8; 8];
            assert!(emu.read_memory(PAGE, &mut buf), "{tag} read0");
            eprintln!("{tag} [0]={}", u64::from_le_bytes(buf));
            assert_eq!(u64::from_le_bytes(buf), 1, "{tag} lost pre-protect write (sharing broke)");
            assert!(emu.read_memory(PAGE + 8, &mut buf), "{tag} read8");
            assert_eq!(u64::from_le_bytes(buf), 2, "{tag} lost post-protect write (sharing broke)");
        };
        check(&mut a, "A");
        check(&mut b, "B");
    }

    #[test]
    fn concurrent_guest_read_sees_peer_host_write() {
        use icicle_vm::cpu::mem::perm;
        use std::sync::Arc;
        use std::time::Duration;

        const PAGE: u64 = 0x40000;
        const TARGET: u64 = PAGE + 0x100; // A host-writes this
        const PROBE: u64 = PAGE + 0x108;  // B stores its loaded rax here each poll

        let shared: Arc<_> = {
            let mut seed = IcicleEmulator::new();
            assert!(seed.vm.cpu.mem.map_smp_shared_fresh(PAGE, perm::READ | perm::WRITE | perm::EXEC));
            // B's poll loop ON THE SHARED PAGE:
            //   0x00 mov rax,[TARGET] (48 A1 moffs64) | 0x0A mov [PROBE],rax (48 A3 moffs64)
            //   0x14 test rax,rax      (48 85 C0)    | 0x17 jz 0x00 (74 E7) | 0x19 jmp $ (EB FE)
            let mut code = Vec::new();
            code.extend_from_slice(&[0x48, 0xA1]);
            code.extend_from_slice(&TARGET.to_le_bytes());
            code.extend_from_slice(&[0x48, 0xA3]);
            code.extend_from_slice(&PROBE.to_le_bytes());
            code.extend_from_slice(&[0x48, 0x85, 0xC0]);
            code.extend_from_slice(&[0x74, 0xE7]);
            code.extend_from_slice(&[0xEB, 0xFE]);
            assert!(seed.write_memory(PAGE, &code));
            seed.vm.cpu.mem.share_page(PAGE).expect("shared arc")
        };

        let w_page = Arc::clone(&shared);
        let writer = std::thread::spawn(move || {
            let mut a = IcicleEmulator::new();
            assert!(a.vm.cpu.mem.map_smp_shared(PAGE, w_page));
            std::thread::sleep(Duration::from_millis(100)); // let B's loop get hot (jitted)
            assert!(a.write_memory(TARGET, &0xfeedfacefeedfaceu64.to_le_bytes()));
        });

        let r_page = Arc::clone(&shared);
        let reader = std::thread::spawn(move || {
            let mut b = IcicleEmulator::new();
            assert!(b.vm.cpu.mem.map_smp_shared(PAGE, Arc::clone(&r_page)));
            let ptr_eq = |b: &mut IcicleEmulator| -> bool {
                b.vm.cpu.mem.share_page(PAGE).map(|p| Arc::ptr_eq(&p, &r_page)).unwrap_or(false)
            };
            b.vm.cpu.write_pc(PAGE);
            b.start(1);
            for _ in 0..2000 {
                b.start(100_000);
                let mut buf = [0u8; 8];
                assert!(b.read_memory(PROBE, &mut buf));
                if u64::from_le_bytes(buf) != 0 {
                    return true;
                }
            }
            false
        });

        writer.join().unwrap();
        assert!(
            reader.join().unwrap(),
            "B's jitted poll loop never observed A's concurrent write (PROBE stayed 0)"
        );
    }

    /// 6.6 core question: cross-vCPU self-modifying code. VM A translates + executes code on a
    /// SHARED page; VM B rewrites that code (legitimate SMC, e.g. a patching thread); VM A must then
    /// execute the NEW bytes, not its stale translation.
    // IGNORED = open 6.6 regression target: a peer VM's write to a TRANSLATED shared page must
    // invalidate the executing VM's translation. Currently VM A keeps executing its stale block
    // (rax=0x1111 after the peer wrote 0x2222 code). This is the cross-vCPU half of icicle's
    // same-VM SMC detection: the shared PageData perm carries IN_CODE_CACHE in place (write_ptr
    // fix), but the peer's write only clears/invalidates ITS OWN VM's TLB + code cache. Fix
    // direction: fan the invalidation out to every VM sharing the page (queued peer op calling an
    // exposed invalidate-range on each handle, plus a generation/epoch check for guest-TLB writes).
    // IGNORED: the bridge is a single-VM view — the cross-VM invalidation fan-out lives in the C++
    // backend (it owns all N VMs + the peer queues), so this cannot pass at the Rust level.
    // The real gate is IcicleSmp.CrossVcpuSelfModifyingCodeInvalidatesPeerTranslation.
    #[test]
    #[ignore]
    fn cross_vcpu_self_modifying_code_is_seen_by_executing_vm() {
        use icicle_vm::cpu::mem::perm;
        const PAGE: u64 = 0x40000;
        let mut a = IcicleEmulator::new();
        let mut b = IcicleEmulator::new();
        assert!(a.vm.cpu.mem.map_smp_shared_fresh(PAGE, perm::READ | perm::WRITE | perm::EXEC));
        let shared = a.vm.cpu.mem.share_page(PAGE).expect("shared arc");
        assert!(b.vm.cpu.mem.map_smp_shared(PAGE, shared));

        // v1: mov eax, 0x1111 (B8 11 11 00 00); jmp $-5 (EB F9)   -> rax accumulates 0x1111s
        // v2: mov eax, 0x2222 (B8 22 22 00 00); jmp $-5
        let v1: [u8; 7] = [0xB8, 0x11, 0x11, 0x00, 0x00, 0xEB, 0xF9];
        let v2: [u8; 7] = [0xB8, 0x22, 0x22, 0x00, 0x00, 0xEB, 0xF9];

        assert!(a.write_memory(PAGE, &v1));
        a.vm.cpu.write_pc(PAGE);
        a.start(10); // translate + execute v1
        let read_rax = |emu: &mut IcicleEmulator| -> u64 {
            let mut buf = [0u8; 8];
            emu.read_register(registers::X86Register::Rax, &mut buf);
            u64::from_le_bytes(buf)
        };
        let after_v1 = read_rax(&mut a);
        println!("STEP6_1 after executing v1: rax={after_v1:#x} (expect 0x1111)");
        assert_eq!(after_v1, 0x1111);

        // VM B rewrites the shared page (SMC through the peer VM).
        println!("STEP6_2 VM B writes v2 over the shared code page");
        assert!(b.write_memory(PAGE, &v2));

        // VM A keeps executing: it must pick up v2 (fresh translation), not its stale v1 block.
        a.vm.cpu.write_pc(PAGE);
        a.start(10);
        let after_v2 = read_rax(&mut a);
        println!("STEP6_3 after SMC + re-execute: rax={after_v2:#x} (expect 0x2222, stale=0x1111)");
        assert_eq!(after_v2, 0x2222, "VM A executed its STALE translation after peer SMC");
    }

    #[test]
    fn warmed_data_write_tlb_survives_exec_promotion_and_repeated_smc() {
        use icicle_vm::cpu::mem::perm;

        const TARGET: u64 = 0x50000;
        const WRITER: u64 = 0x60000;
        let mut reader = IcicleEmulator::new();
        let mut writer = IcicleEmulator::new();
        assert!(reader.vm.cpu.mem.map_smp_shared_fresh(TARGET, perm::READ | perm::WRITE));
        let shared = reader.vm.cpu.mem.share_page(TARGET).unwrap();
        assert!(writer.vm.cpu.mem.map_smp_shared(TARGET, shared.clone()));
        assert!(writer.map_memory(WRITER, 0x1000, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));

        let mut code = [0u8; 25];
        code[..2].copy_from_slice(&[0x48, 0xB8]);
        code[2..10].copy_from_slice(&TARGET.to_le_bytes());
        code[10..12].copy_from_slice(&[0x48, 0xB9]);
        code[12..20].copy_from_slice(&0x90F9EB00001111B8u64.to_le_bytes());
        code[20..].copy_from_slice(&[0x48, 0x89, 0x08, 0xEB, 0xFE]);
        assert!(writer.write_memory(WRITER, &code));
        writer.vm.cpu.write_pc(WRITER);
        writer.start(4);

        let fast = std::env::var("SOGEN_SMP_FAST_WRITE_EPOCH").as_deref() == Ok("1");
        assert_eq!(writer.vm.cpu.mem.tlb.translate_write(TARGET).is_some(), fast);
        assert!(reader.vm.cpu.mem.update_perm(TARGET, 0x1000, perm::READ | perm::WRITE | perm::EXEC).is_ok());
        assert_eq!(writer.vm.cpu.mem.tlb.translate_write(TARGET).is_some(), fast);

        let read_eax = |emu: &mut IcicleEmulator| {
            let mut buf = [0u8; 8];
            emu.read_register(registers::X86Register::Rax, &mut buf);
            u64::from_le_bytes(buf)
        };
        reader.vm.cpu.write_pc(TARGET);
        reader.start(10);
        assert_eq!(read_eax(&mut reader), 0x1111);

        for value in [0x2222u64, 0x3333u64] {
            let epoch_before = shared.code_epoch();
            let instruction = 0x90F9EB00000000B8u64 | (value << 8);
            writer.write_register(registers::X86Register::Rax, &TARGET.to_le_bytes());
            writer.write_register(registers::X86Register::Rcx, &instruction.to_le_bytes());
            writer.vm.cpu.write_pc(WRITER + 20);
            writer.start(3);
            assert!(shared.code_epoch() > epoch_before);
            reader.vm.cpu.write_pc(TARGET);
            reader.start(10);
            assert_eq!(read_eax(&mut reader), value);
        }
    }

    #[test]
    fn two_vms_share_fast_smp_page() {
        use icicle_vm::cpu::mem::perm;
        let mut a = IcicleEmulator::new();
        let mut b = IcicleEmulator::new();
        let addr = 0x40000u64;
        // A creates a fresh shared page (cachable, write-through); B maps the same Arc backing.
        assert!(a.vm.cpu.mem.map_smp_shared_fresh(addr, perm::READ | perm::WRITE));
        let shared = a.vm.cpu.mem.share_page(addr).expect("shared arc");
        assert!(b.vm.cpu.mem.map_smp_shared(addr, shared));
        // Write via A -> visible via B (shared bytes, no copy-on-write).
        assert!(a.write_memory(addr, &0xa1b2c3d4u32.to_le_bytes()));
        let mut buf = [0u8; 4];
        assert!(b.read_memory(addr, &mut buf));
        assert_eq!(u32::from_le_bytes(buf), 0xa1b2c3d4, "SMP write via A must be visible via B");
        // Write via B -> visible via A.
        assert!(b.write_memory(addr + 8, &0x55667788u32.to_le_bytes()));
        let mut buf2 = [0u8; 4];
        assert!(a.read_memory(addr + 8, &mut buf2));
        assert_eq!(u32::from_le_bytes(buf2), 0x55667788, "SMP write via B must be visible via A");
    }

    #[test]
    fn warmed_peer_write_tlb_is_retired_before_first_decode() {
        use icicle_vm::cpu::mem::perm;
        use std::sync::mpsc;

        if std::env::var("SOGEN_SMP_PROMOTION_RACE_TEST").as_deref() != Ok("1") {
            return;
        }
        assert_eq!(std::env::var("SOGEN_ICICLE_JIT").as_deref(), Ok("1"));

        const TARGET: u64 = 0x50000;
        const WRITER: u64 = 0x60000;
        let mut reader = IcicleEmulator::new_with_instruction_hooks(false);
        assert!(reader.vm.cpu.mem.map_smp_shared_fresh(TARGET, perm::READ | perm::WRITE));
        assert!(reader.write_memory(TARGET, &[0xB8, 0x11, 0x11, 0, 0, 0xEB, 0xFE]));
        let shared = reader.vm.cpu.mem.share_page(TARGET).unwrap();
        let (ready_tx, ready_rx) = mpsc::channel();
        let (proceed_tx, proceed_rx) = mpsc::channel();
        let worker = std::thread::spawn(move || {
            let mut writer = IcicleEmulator::new_with_instruction_hooks(false);
            assert!(writer.map_memory(WRITER, 0x1000, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));
            let mut code = [0u8; 16];
            code[..2].copy_from_slice(&[0x48, 0xB8]);
            code[2..10].copy_from_slice(&(TARGET + 0x80).to_le_bytes());
            code[10..].copy_from_slice(&[0xC7, 0x00, 0x78, 0x56, 0x34, 0x12]);
            assert!(writer.write_memory(WRITER, &code));
            assert!(writer.vm.cpu.mem.map_smp_shared(TARGET, shared));
            writer.vm.cpu.write_pc(WRITER);
            writer.start(2);
            let warmed = writer.vm.cpu.mem.tlb.translate_write(TARGET + 0x80).is_some();
            ready_tx.send(warmed).unwrap();
            proceed_rx.recv().unwrap();
            writer.vm.cpu.mem.tlb.translate_write(TARGET + 0x80).is_some()
        });

        let warmed = ready_rx.recv().unwrap();
        let fast_write = std::env::var("SOGEN_SMP_FAST_WRITE_EPOCH").as_deref() == Ok("1");
        assert_eq!(warmed, fast_write);
        let index = reader.vm.cpu.mem.get_physical_index(TARGET).unwrap();
        assert!(!reader.vm.cpu.mem.get_physical(index).executed);
        reader.vm.cpu.mem.update_perm(TARGET, 0x1000, perm::READ | perm::WRITE | perm::EXEC).unwrap();
        proceed_tx.send(()).unwrap();
        let peer_write_tlb = worker.join().unwrap();

        reader.vm.cpu.write_pc(TARGET);
        reader.start(2);
        let mut rax = [0u8; 8];
        reader.read_register(registers::X86Register::Rax, &mut rax);
        assert_eq!(u64::from_le_bytes(rax), 0x1111);
        println!("first_decode_peer_write_tlb={} fast_write={}", u8::from(peer_write_tlb), u8::from(fast_write));
        assert!(!peer_write_tlb, "peer can directly write shared executable bytes before first decode");
    }

    #[cfg(windows)]
    fn smp_store_bench_thread_cpu_seconds() -> f64 {
        #[repr(C)]
        #[derive(Clone, Copy, Default)]
        struct FileTime {
            low: u32,
            high: u32,
        }

        #[link(name = "kernel32")]
        unsafe extern "system" {
            fn GetCurrentThread() -> *mut std::ffi::c_void;
            fn GetThreadTimes(
                thread: *mut std::ffi::c_void,
                creation: *mut FileTime,
                exit: *mut FileTime,
                kernel: *mut FileTime,
                user: *mut FileTime,
            ) -> i32;
        }

        let mut creation = FileTime::default();
        let mut exit = FileTime::default();
        let mut kernel = FileTime::default();
        let mut user = FileTime::default();
        let ok = unsafe {
            GetThreadTimes(
                GetCurrentThread(),
                &mut creation,
                &mut exit,
                &mut kernel,
                &mut user,
            )
        };
        assert_ne!(ok, 0);
        let ticks = |time: FileTime| ((time.high as u64) << 32) | time.low as u64;
        (ticks(kernel) + ticks(user)) as f64 / 10_000_000.0
    }

    #[cfg(not(windows))]
    fn smp_store_bench_thread_cpu_seconds() -> f64 {
        f64::NAN
    }

    #[test]
    fn smp_shared_data_store_epoch_bench() {
        use icicle_vm::cpu::mem::perm;
        use std::sync::{Arc, Barrier};
        use std::time::Instant;

        if std::env::var("SOGEN_SMP_STORE_BENCH").as_deref() != Ok("1") {
            return;
        }
        assert_eq!(std::env::var("SOGEN_ICICLE_JIT").as_deref(), Ok("1"));
        let fast_write = match std::env::var("SOGEN_SMP_FAST_WRITE_EPOCH").as_deref() {
            Ok("0") => false,
            Ok("1") => true,
            _ => panic!("set SOGEN_SMP_FAST_WRITE_EPOCH to 0 or 1"),
        };
        assert!(!smp_lean_epoch_hook_enabled());
        let measured_icount = std::env::var("SOGEN_SMP_STORE_BENCH_ICOUNT")
            .ok()
            .and_then(|value| value.parse::<u64>().ok())
            .unwrap_or(100_000_000);
        assert!(measured_icount >= 3_000_000);

        const CODE: u64 = 0x10000;
        const DATA: u64 = 0x50000;
        let shared = {
            let mut seed = IcicleEmulator::new_with_instruction_hooks(false);
            assert!(seed.vm.cpu.mem.map_smp_shared_fresh(DATA, perm::READ | perm::WRITE));
            seed.vm.cpu.mem.share_page(DATA).unwrap()
        };
        let epoch_mode = std::env::var("SOGEN_SMP_CODE_EPOCH_ONLY").expect("set SOGEN_SMP_CODE_EPOCH_ONLY to 0 or 1");
        assert!(epoch_mode == "0" || epoch_mode == "1");
        let epoch_only = epoch_mode == "1";
        assert_eq!(
            shared.smp_code_seen.load(std::sync::atomic::Ordering::Acquire),
            u8::from(!epoch_only),
        );

        let ready = Arc::new(Barrier::new(3));
        let go = Arc::new(Barrier::new(3));
        let handles: Vec<_> = (0usize..2).map(|index| {
            let page = Arc::clone(&shared);
            let ready = Arc::clone(&ready);
            let go = Arc::clone(&go);
            std::thread::spawn(move || {
                let mut emu = IcicleEmulator::new_with_instruction_hooks(false);
                assert!(emu.map_memory(CODE, 0x1000, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));
                assert!(emu.write_memory(CODE, &[0x89, 0x08, 0xFF, 0xC1, 0xEB, 0xFA]));
                assert!(emu.vm.cpu.mem.map_smp_shared(DATA, page));
                let slot = DATA + (index as u64) * 256;
                emu.write_register(registers::X86Register::Rax, &slot.to_le_bytes());
                emu.vm.cpu.write_pc(CODE);
                emu.start(3_000_000);
                let fast_tlb = emu.vm.cpu.mem.tlb.translate_write(slot).is_some();
                ready.wait();
                go.wait();

                let before_icount = emu.vm.cpu.icount;
                let before_misses = emu.vm.cpu.mem.tlb_miss_count;
                let before_cpu = smp_store_bench_thread_cpu_seconds();
                let start = Instant::now();
                emu.start(measured_icount);
                (
                    emu.vm.cpu.icount - before_icount,
                    start.elapsed().as_secs_f64(),
                    smp_store_bench_thread_cpu_seconds() - before_cpu,
                    fast_tlb,
                    emu.vm.cpu.mem.tlb_miss_count - before_misses,
                )
            })
        }).collect();

        ready.wait();
        let epoch_before = shared.code_epoch();
        let start = Instant::now();
        go.wait();
        let results: Vec<_> = handles.into_iter().map(|handle| handle.join().unwrap()).collect();
        let wall = start.elapsed().as_secs_f64();
        let epoch_delta = shared.code_epoch() - epoch_before;
        for index in 0..2 {
            let offset = index * 256;
            let value = u32::from_le_bytes(shared.data[offset..offset + 4].try_into().unwrap());
            assert!(value > 0);
            assert_eq!(results[index].3, fast_write);
        }
        if epoch_only {
            assert_eq!(epoch_delta, 0);
        } else {
            assert!(epoch_delta > 0);
        }

        let retired: u64 = results.iter().map(|result| result.0).sum();
        let worker_cpu: f64 = results.iter().map(|result| result.2).sum();
        let tlb_misses: u64 = results.iter().map(|result| result.4).sum();
        println!(
            "mode={} fast_write={} wall={wall:.6}s worker_cpu={worker_cpu:.6}s retired={retired} epoch_delta={epoch_delta} tlb_misses={tlb_misses} aggregate_mips={:.3} per_vcpu_mips={:.3} store_mops={:.3} worker_wall=[{:.6},{:.6}]",
            if epoch_only { "exec_only" } else { "all_shared" },
            u8::from(fast_write),
            retired as f64 / wall / 1e6,
            retired as f64 / wall / 2e6,
            (retired / 3) as f64 / wall / 1e6,
            results[0].1,
            results[1].1,
        );
    }

    /// The multi-core payoff: N per-vCPU VMs execute concurrently on ONE shared page (each writing
    /// its own 8-byte slot), correct and near-linear. Gated on SOGEN_BENCH_OUT.
    #[test]
    fn smp_concurrent_scaling() {
        use icicle_vm::cpu::mem::perm;
        use std::sync::Arc;
        use std::time::Instant;
        let out = match std::env::var("SOGEN_BENCH_OUT") { Ok(p) => p, Err(_) => return };
        let icount: u64 = std::env::var("SOGEN_BENCH_ICOUNT")
            .ok().and_then(|v| v.parse().ok()).unwrap_or(300_000_000);
        const CODE: u64 = 0x10000;
        const DATA: u64 = 0x50000;
        let mut lines = vec![format!(
            "SMP concurrent execution: N per-vCPU VMs share ONE page, each stores to its slot | icount/vcpu={}",
            icount)];
        let mut baseline = 0.0f64;
        for &n in &[1usize, 2, 4, 8] {
            // One shared page, created on a seed VM then handed to every vCPU.
            let shared: Arc<_> = {
                let mut seed = IcicleEmulator::new();
                assert!(seed.vm.cpu.mem.map_smp_shared_fresh(DATA, perm::READ | perm::WRITE));
                seed.vm.cpu.mem.share_page(DATA).expect("shared arc")
            };
            let wall = Instant::now();
            let handles: Vec<_> = (0..n).map(|i| {
                let page = Arc::clone(&shared);
                std::thread::spawn(move || {
                    let mut emu = IcicleEmulator::new();
                    assert!(emu.map_memory(CODE, 0x1000, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));
                    // mov [rax],ecx; inc ecx; jmp $-6  (store counter to this vCPU's slot each iter)
                    assert!(emu.write_memory(CODE, &[0x89, 0x08, 0xFF, 0xC1, 0xEB, 0xFA]));
                    assert!(emu.vm.cpu.mem.map_smp_shared(DATA, page));
                    // 256-byte spacing so each vCPU's slot is on its own cache line (no false sharing).
                    emu.write_register(registers::X86Register::Rax, &(DATA + (i as u64) * 256).to_le_bytes());
                    emu.vm.cpu.write_pc(CODE);
                    let before = emu.vm.cpu.icount;
                    emu.start(icount);
                    emu.vm.cpu.icount - before
                })
            }).collect();
            let total: u64 = handles.into_iter().map(|h| h.join().unwrap()).sum();
            let wall = wall.elapsed().as_secs_f64();
            // Correctness: every vCPU wrote its own slot on the shared page (no corruption).
            for i in 0..n {
                let off = i * 256;
                let v = u32::from_le_bytes(shared.data[off..off + 4].try_into().unwrap());
                assert!(v > 0, "vCPU {i} did not write its shared slot (got {v})");
            }
            let agg = total as f64 / wall / 1e6;
            let pt = agg / n as f64;
            if n == 1 { baseline = pt; }
            let eff = if baseline > 0.0 { pt / baseline } else { 1.0 };
            lines.push(format!(
                "vcpus={:>2}  wall={:>7.3}s  aggregate={:>8.1} MIPS  per_vcpu={:>7.1} MIPS  eff={:.2}x",
                n, wall, agg, pt, eff));
        }
        let report = lines.join("\n") + "\n";
        print!("\n{}", report);
        if let Some(parent) = std::path::Path::new(&out).parent() { let _ = std::fs::create_dir_all(parent); }
        std::fs::write(&out, &report).expect("write bench results");
    }
}

#[cfg(test)]
mod invalidation_profile_tests {
    use super::*;
    use std::sync::atomic::Ordering;

    #[test]
    fn jit_profile_attributes_compiles_to_full_reset_generations() {
        const PAGE: u64 = 0x76000;
        let mut emu = IcicleEmulator::new_with_instruction_hooks(false);
        emu.jit_profile_enabled = true;
        emu.vm.jit.profile_enabled = true;
        emu.vm.enable_jit = true;
        assert!(emu.map_memory(PAGE, 0x1000, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));
        assert!(emu.write_memory(PAGE, &[0x90, 0xeb, 0xfe]));
        emu.vm.cpu.write_pc(PAGE);
        emu.start(8);
        let first = emu.jit_profile();
        assert_eq!(first.reset_generation, 0);
        assert!(first.generation_compile_calls > 0);
        assert_eq!(first.generation_compile_calls, first.compile_calls);

        emu.mark_manual_origin(MANUAL_SELF_MODIFYING);
        emu.invalidate_code.set(true);
        emu.flush_pending_code();
        let reset = emu.jit_profile();
        assert_eq!(reset.reset_generation, 1);
        assert_eq!(reset.reset_cause_flags, INVALIDATION_MANUAL as u64);
        assert_eq!(reset.reset_manual_origin_flags, MANUAL_SELF_MODIFYING as u64);
        assert_eq!(reset.generation_compile_calls, 0);
        assert_eq!(reset.generation_compile_nanos, 0);
        assert_eq!(reset.reset_calls, 1);

        emu.start(8);
        let second = emu.jit_profile();
        assert_eq!(second.reset_generation, 1);
        assert!(second.generation_compile_calls > 0);
        assert_eq!(second.generation_compile_calls, second.compile_calls - first.compile_calls);
    }

    #[test]
    fn distinguishes_epoch_wake_manual_and_coalesced_jit_resets() {
        if smp_epoch_mode() == 0 {
            return;
        }
        use icicle_vm::cpu::mem::perm;

        const PAGE: u64 = 0x75000;
        let mut emu = IcicleEmulator::new_with_instruction_hooks(false);
        let profile = std::sync::Arc::new(InvalidationProfile::default());
        let causes = Rc::new(Cell::new(0));
        emu.invalidation_profile = Some(profile.clone());
        emu.invalidation_causes = Rc::clone(&causes);
        {
            let mut hooks = emu.execution_hooks.borrow_mut();
            hooks.invalidation_profile = Some(profile.clone());
            hooks.invalidation_causes = Some(Rc::clone(&causes));
        }
        assert!(emu.vm.cpu.mem.map_smp_shared_fresh(PAGE, perm::READ | perm::WRITE | perm::EXEC));
        let shared = emu.vm.cpu.mem.share_page(PAGE).unwrap();

        // Seed the normal first-page epoch, then change it as a peer store would.
        emu.execution_hooks.borrow_mut().check_code_epoch(&mut emu.vm.cpu, PAGE);
        shared.bump_code_epoch();
        emu.execution_hooks.borrow_mut().check_code_epoch(&mut emu.vm.cpu, PAGE);
        assert_eq!(profile.epoch_mismatches.load(Ordering::Relaxed), 1);
        emu.flush_pending_code();
        assert_eq!(profile.epoch_resets.load(Ordering::Relaxed), 1);

        emu.exec_write_pending.store(true, Ordering::Release);
        emu.reconcile_exec_write_wake();
        assert_eq!(profile.wake_resets.load(Ordering::Relaxed), 1);

        assert!(emu.vm.cpu.mem.ensure_executable(PAGE, 1));
        assert!(emu.invalidate_code_range(PAGE, 1, MANUAL_PUBLIC_INVALIDATE));
        emu.flush_pending_code();
        assert_eq!(profile.manual_resets.load(Ordering::Relaxed), 1);
        assert_eq!(profile.manual_origin_resets.load(Ordering::Relaxed), 1);
        assert_eq!(profile.manual_public_invalidate.load(Ordering::Relaxed), 1);

        assert!(emu.vm.cpu.mem.ensure_executable(PAGE, 1));
        assert!(emu.invalidate_code_range(PAGE, 1, MANUAL_PUBLIC_INVALIDATE));
        emu.exec_write_pending.store(true, Ordering::Release);
        emu.reconcile_exec_write_wake();
        assert_eq!(profile.mixed_resets.load(Ordering::Relaxed), 1);
        assert_eq!(profile.manual_origin_resets.load(Ordering::Relaxed), 2);
        assert_eq!(profile.manual_public_invalidate.load(Ordering::Relaxed), 2);

        assert!(emu.vm.cpu.mem.ensure_executable(PAGE, 1));
        emu.refresh_peer_protection(PAGE, 1);
        emu.flush_pending_code();
        assert_eq!(profile.manual_peer_protection.load(Ordering::Relaxed), 1);

        assert!(emu.vm.cpu.mem.ensure_executable(PAGE, 1));
        assert!(emu.invalidate_code_range(PAGE, 1, MANUAL_PUBLIC_INVALIDATE));
        assert!(emu.vm.cpu.mem.ensure_executable(PAGE, 1));
        emu.refresh_peer_protection(PAGE, 1);
        emu.flush_pending_code();
        assert_eq!(profile.manual_multiple_origins.load(Ordering::Relaxed), 1);

        emu.invalidate_code.set(true);
        emu.flush_pending_code();
        assert_eq!(profile.unknown_resets.load(Ordering::Relaxed), 1);
        assert_eq!(profile.jit_resets.load(Ordering::Relaxed), 7);
        assert_eq!(profile.manual_resets.load(Ordering::Relaxed), 3);
        assert_eq!(profile.manual_origin_resets.load(Ordering::Relaxed), 4);
        assert_eq!(profile.manual_unknown_origin.load(Ordering::Relaxed), 0);
        assert_eq!(emu.exec_write_flushes.load(Ordering::Relaxed), 2);
    }

    #[test]
    fn manual_origin_buckets_are_exclusive_and_bounded() {
        let profile = InvalidationProfile::default();
        for origin in [
            MANUAL_PEER_PROTECTION,
            MANUAL_PUBLIC_INVALIDATE,
            MANUAL_SELF_MODIFYING,
            MANUAL_HOST_CACHE,
            MANUAL_UNMAP,
            MANUAL_PROTECT,
            MANUAL_HOST_WRITE,
            MANUAL_PUBLIC_INVALIDATE | MANUAL_PROTECT,
            0,
        ] {
            profile.record_reset(INVALIDATION_MANUAL, origin);
        }
        assert_eq!(profile.manual_origin_resets.load(Ordering::Relaxed), 9);
        for count in [
            &profile.manual_peer_protection,
            &profile.manual_public_invalidate,
            &profile.manual_self_modifying,
            &profile.manual_host_cache,
            &profile.manual_unmap,
            &profile.manual_protect,
            &profile.manual_host_write,
            &profile.manual_multiple_origins,
            &profile.manual_unknown_origin,
        ] {
            assert_eq!(count.load(Ordering::Relaxed), 1);
        }
        assert_eq!(profile.manual_resets.load(Ordering::Relaxed), 9);
        profile.record_reset(INVALIDATION_EPOCH, 0);
        assert_eq!(profile.manual_origin_resets.load(Ordering::Relaxed), 9);
    }
}

#[cfg(test)]
mod parallel_scaling_bench {
    //! Does execution scale across OS threads? Each thread builds its OWN `IcicleEmulator`
    //! (created in-thread, never moved, so the `Rc<RefCell>` interior stays single-threaded) and
    //! runs a tight `inc rax; jmp` loop for a fixed instruction budget with no memory traffic.
    //! Near-linear aggregate MIPS proves independent VMs execute concurrently -> the per-vCPU
    //! *instance* multi-core design is viable and the remaining work is a shared, thread-safe
    //! memory backing (icicle-mem). Sub-linear scaling would expose a process-global bottleneck
    //! (allocator, JIT global state) that must be fixed first. Gated on SOGEN_BENCH_OUT so the
    //! normal `cargo test` run stays fast; results are written there and also printed.
    use super::*;
    use std::time::Instant;

    fn run_loop(code: &[u8], icount: u64) -> (u64, f64) {
        let mut emu = IcicleEmulator::new();
        assert!(emu.map_memory(0x10000, 4096, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));
        assert!(emu.write_memory(0x10000, code));
        emu.vm.cpu.write_pc(0x10000);
        let before = emu.vm.cpu.icount;
        let start = Instant::now();
        emu.start(icount);
        (emu.vm.cpu.icount - before, start.elapsed().as_secs_f64())
    }

    /// tiny: inc rax; jmp $-5  => 2 instrs/block, one dispatch every 2 instrs (dispatch-bound).
    fn tiny_loop() -> Vec<u8> { vec![0x48, 0xFF, 0xC0, 0xEB, 0xFB] }

    /// big: 1000x inc rax; jmp rel32 back => 1001 instrs/block, one dispatch per 1001 instrs
    /// (steady-state JIT throughput). rel32 = -(3000+5) = -3005 = 0xFFFFF443.
    fn big_loop() -> Vec<u8> {
        let mut code = [0x48u8, 0xFF, 0xC0].repeat(1000);
        code.extend_from_slice(&[0xE9, 0x43, 0xF4, 0xFF, 0xFF]);
        code
    }

    /// Same loop on a raw `create_x64_vm()` Vm with NO IcicleEmulator hooks installed: the JIT's
    /// true throughput ceiling. Comparing this to run_loop isolates the always-on per-instruction
    /// hook cost.
    fn run_raw_jit(code: &[u8], icount: u64, jit: bool) -> (u64, f64) {
        use icicle_vm::cpu::mem::{perm, Mapping};
        let mut vm = create_x64_vm();
        vm.enable_jit = jit;
        vm.cpu.mem.set_capacity(8 * 2 * 50_000);
        let p = map_permissions(FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC) | perm::MAP | perm::INIT;
        assert!(vm.cpu.mem.map_memory_len(0x10000, 4096, Mapping { perm: p, value: 0 }));
        vm.cpu.mem.write_bytes(0x10000, code, perm::NONE).unwrap();
        vm.cpu.write_pc(0x10000);
        let before = vm.cpu.icount;
        vm.icount_limit = vm.cpu.icount.saturating_add(icount);
        let start = Instant::now();
        let _ = vm.run();
        (vm.cpu.icount - before, start.elapsed().as_secs_f64())
    }
    fn run_raw(code: &[u8], icount: u64) -> (u64, f64) { run_raw_jit(code, icount, true) }
    fn run_raw_nojit(code: &[u8], icount: u64) -> (u64, f64) { run_raw_jit(code, icount, false) }

    /// Store-loop (`mov [rax],ecx; inc ecx; jmp`) writing to a data page each iteration. When
    /// `host_mapped`, the data page is a borrowed host buffer (external, bypasses the JIT direct
    /// TLB) as SMP shared RAM would be; otherwise a normal page. Ratio = the external-memory tax.
    // mode: 0 = normal page, 1 = host-mapped (external), 2 = SMP-shared (cachable write-through)
    fn run_store_loop(mode: u8, icount: u64) -> (u64, f64) {
        let mut emu = IcicleEmulator::new();
        assert!(emu.map_memory(0x10000, 0x1000, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));
        assert!(emu.write_memory(0x10000, &[0x89, 0x08, 0xFF, 0xC1, 0xEB, 0xFA]));
        let host = match mode {
            1 => {
                let layout = std::alloc::Layout::from_size_align(0x1000, 0x1000).unwrap();
                let p = unsafe { std::alloc::alloc_zeroed(layout) };
                assert!(!p.is_null());
                unsafe { assert!(emu.map_host_memory(0x30000, p, 0x1000, FOREIGN_READ | FOREIGN_WRITE)) };
                Some((p, layout))
            }
            2 => {
                use icicle_vm::cpu::mem::perm;
                assert!(emu.vm.cpu.mem.map_smp_shared_fresh(0x30000, perm::READ | perm::WRITE));
                None
            }
            _ => {
                assert!(emu.map_memory(0x30000, 0x1000, FOREIGN_READ | FOREIGN_WRITE));
                None
            }
        };
        emu.write_register(registers::X86Register::Rax, &0x30000u64.to_le_bytes());
        emu.vm.cpu.write_pc(0x10000);
        let before = emu.vm.cpu.icount;
        let start = Instant::now();
        emu.start(icount);
        let out = (emu.vm.cpu.icount - before, start.elapsed().as_secs_f64());
        drop(emu);
        if let Some((p, layout)) = host { unsafe { std::alloc::dealloc(p, layout) } }
        out
    }

    #[test]
    fn external_page_speed() {
        let out = match std::env::var("SOGEN_BENCH_OUT") { Ok(p) => p, Err(_) => return };
        let icount: u64 = std::env::var("SOGEN_BENCH_ICOUNT")
            .ok().and_then(|v| v.parse().ok()).unwrap_or(300_000_000);
        let (_, tn) = run_store_loop(0, icount);
        let (_, th) = run_store_loop(1, icount);
        let (_, ts) = run_store_loop(2, icount);
        let mips = |t: f64| icount as f64 / t / 1e6;
        let report = format!(
            "store-loop by page type | icount={}\nnormal page:      {:8.1} MIPS ({:.3}s)\nhost-mapped:      {:8.1} MIPS ({:.3}s)  [{:.0}x slower than normal]\nSMP-shared:       {:8.1} MIPS ({:.3}s)  [{:.2}x vs normal]\n",
            icount, mips(tn), tn, mips(th), th, th / tn, mips(ts), ts, tn / ts);
        print!("\n{}", report);
        if let Some(parent) = std::path::Path::new(&out).parent() { let _ = std::fs::create_dir_all(parent); }
        std::fs::write(&out, &report).expect("write bench results");
    }

    fn scale(lines: &mut Vec<String>, tag: &str, runner: fn(&[u8], u64) -> (u64, f64),
             code: &[u8], icount: u64, counts: &[usize]) {
        lines.push(format!("--- {} ---", tag));
        let mut baseline = 0.0f64;
        for &n in counts {
            let wall = Instant::now();
            let handles: Vec<_> = (0..n)
                .map(|_| { let c = code.to_vec(); std::thread::spawn(move || runner(&c, icount)) })
                .collect();
            let per: Vec<(u64, f64)> = handles.into_iter().map(|h| h.join().unwrap()).collect();
            let wall = wall.elapsed().as_secs_f64();
            let total: u64 = per.iter().map(|(c, _)| *c).sum();
            let agg = total as f64 / wall / 1e6;
            let pt = agg / n as f64;
            if n == 1 { baseline = pt; }
            let eff = if baseline > 0.0 { pt / baseline } else { 1.0 };
            lines.push(format!(
                "threads={:>2}  wall={:>7.3}s  aggregate={:>8.1} MIPS  per_thread={:>7.1} MIPS  eff={:.2}x",
                n, wall, agg, pt, eff));
        }
    }

    #[test]
    fn parallel_scaling() {
        let out = match std::env::var("SOGEN_BENCH_OUT") {
            Ok(path) => path,
            Err(_) => return, // no-op unless explicitly enabled
        };
        let icount: u64 = std::env::var("SOGEN_BENCH_ICOUNT")
            .ok().and_then(|v| v.parse().ok()).unwrap_or(300_000_000);
        let counts: Vec<usize> = std::env::var("SOGEN_BENCH_THREADS")
            .ok().map(|v| v.split(',').filter_map(|x| x.trim().parse().ok()).collect())
            .unwrap_or_else(|| vec![1, 2, 4, 8]);

        let (tiny, big) = (tiny_loop(), big_loop());
        let mut lines = vec![format!(
            "icicle bench | icount/thread={} | host=20 logical cores | HOOKED=IcicleEmulator (analyzer path), RAW=no per-instruction hook",
            icount)];
        scale(&mut lines, "HOOKED tiny(2/blk)", run_loop, &tiny, icount, &counts);
        scale(&mut lines, "HOOKED big(1001/blk)", run_loop, &big, icount, &counts);
        scale(&mut lines, "RAW/no-hook tiny(2/blk)", run_raw, &tiny, icount, &[1, 4]);
        scale(&mut lines, "RAW/no-hook big(1001/blk)", run_raw, &big, icount, &[1, 4]);
        scale(&mut lines, "RAW NOJIT/interpreter tiny(2/blk)", run_raw_nojit, &tiny, icount, &[1]);
        scale(&mut lines, "RAW NOJIT/interpreter big(1001/blk)", run_raw_nojit, &big, icount, &[1]);

        let report = lines.join("\n") + "\n";
        print!("\n{}", report);
        if let Some(parent) = std::path::Path::new(&out).parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        std::fs::write(&out, &report).expect("write bench results");
    }
}

#[cfg(test)]
mod recompilation_flag_tests {
    use super::*;
    use std::process::Command;

    const TEST_NAME: &str = "icicle::recompilation_flag_tests::environment_flag_reaches_vm";
    const PERIODIC_TEST_NAME: &str = "icicle::recompilation_flag_tests::periodic_recompile_honors_flag";

    #[test]
    fn environment_flag_reaches_vm() {
        if std::env::var_os("SOGEN_RECOMP_FLAG_TEST_CHILD").is_some() {
            let expected = std::env::var("SOGEN_ICICLE_RECOMP").as_deref() != Ok("0");
            assert_eq!(create_x64_vm().enable_recompilation, expected);
            return;
        }

        // Child processes keep environment overrides isolated from parallel Rust tests.
        for flag in [Some("0"), Some("1"), None] {
            let mut command = Command::new(std::env::current_exe().unwrap());
            command.args(["--exact", TEST_NAME, "--nocapture"]);
            command.env("SOGEN_RECOMP_FLAG_TEST_CHILD", "1");
            match flag {
                Some(value) => { command.env("SOGEN_ICICLE_RECOMP", value); }
                None => { command.env_remove("SOGEN_ICICLE_RECOMP"); }
            }
            let result = command.output().unwrap();
            assert!(result.status.success(), "flag={flag:?}: {} {}",
                String::from_utf8_lossy(&result.stdout), String::from_utf8_lossy(&result.stderr));
        }
    }

    #[test]
    #[ignore = "waits for Icicle's 60-second recompile threshold; run explicitly"]
    fn periodic_recompile_honors_flag() {
        if std::env::var_os("SOGEN_RECOMP_PERIODIC_TEST_CHILD").is_some() {
            let enabled = std::env::var("SOGEN_ICICLE_RECOMP").as_deref() != Ok("0");
            let mut vm = create_x64_vm();
            assert_eq!(vm.enable_recompilation, enabled);
            vm.compiled_blocks = 11;
            std::thread::sleep(std::time::Duration::from_secs(61));
            assert!(vm.should_recompile());
            vm.icount_limit = 0;
            assert!(matches!(vm.run(), icicle_vm::VmExit::InstructionLimit));
            assert_eq!(vm.compiled_blocks, if enabled { 0 } else { 11 });
            return;
        }

        let mut children = Vec::new();
        for flag in ["0", "1"] {
            let mut command = Command::new(std::env::current_exe().unwrap());
            command.args(["--ignored", "--exact", PERIODIC_TEST_NAME, "--nocapture"]);
            command.env("SOGEN_RECOMP_PERIODIC_TEST_CHILD", "1");
            command.env("SOGEN_ICICLE_RECOMP", flag);
            children.push((flag, command.spawn().unwrap()));
        }
        for (flag, child) in &mut children {
            assert!(child.wait().unwrap().success(), "periodic recompile flag={flag} failed");
        }
    }
}

#[cfg(test)]
mod range_invalidation_tests {
    use super::*;

    fn code(value: u32) -> Vec<u8> {
        let mut bytes = vec![0xb8]; // mov eax, imm32
        bytes.extend_from_slice(&value.to_le_bytes());
        bytes.extend_from_slice(&[0xeb, 0xfe]); // jmp $ (bounded by the test's instruction limit)
        bytes
    }

    fn read_rax(emu: &mut IcicleEmulator) -> u64 {
        let mut bytes = [0; 8];
        emu.read_register(registers::X86Register::Rax, &mut bytes);
        u64::from_le_bytes(bytes)
    }

    fn group_at(emu: &IcicleEmulator, address: u64) -> Option<icicle_cpu::BlockGroup> {
        emu.vm.code.map.iter().find_map(|(key, group)| (key.vaddr == address).then_some(*group))
    }

    #[test]
    fn manual_range_preserves_other_jit_entry_and_recompiles_changed_page() {
        const A: u64 = 0x10000;
        const B: u64 = 0x20000;
        let mut emu = IcicleEmulator::new_with_instruction_hooks(false);
        emu.range_invalidation_enabled = true;
        emu.vm.enable_jit = true;
        assert!(emu.vm.enable_recompilation);
        for (address, value) in [(A, 1), (B, 2)] {
            assert!(emu.map_memory(address, 4096, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));
            assert!(emu.write_memory(address, &code(value)));
            emu.vm.cpu.write_pc(address);
            emu.start(100);
            assert_eq!(read_rax(&mut emu), value as u64);
        }
        assert!(emu.vm.enable_recompilation);
        let a_group = group_at(&emu, A).expect("A was lifted");
        let b_group = group_at(&emu, B).expect("B was lifted");
        let a_func = emu.vm.jit.entry_points[&A];
        assert!(emu.vm.jit.entry_points.contains_key(&B));

        assert!(emu.write_memory(B + 1, &3u32.to_le_bytes()));
        emu.flush_pending_code();
        assert!(emu.vm.enable_recompilation);
        assert_eq!(group_at(&emu, A).unwrap().blocks, a_group.blocks);
        assert!(emu.vm.code.blocks[a_group.blocks.0].entry.is_some());
        assert!(b_group.range().all(|id| emu.vm.code.blocks[id].entry.is_none()));
        assert!(group_at(&emu, B).is_none());
        assert!(std::ptr::fn_addr_eq(emu.vm.jit.entry_points[&A], a_func));
        assert!(!emu.vm.jit.entry_points.contains_key(&B));

        // Force the upstream recompile path, including its purge/reset branch. A removed
        // block must not be reintroduced from the append-only code.blocks array.
        emu.vm.recompile();
        assert!(!emu.vm.jit.entry_points.contains_key(&B));
        emu.vm.cpu.write_pc(A);
        emu.start(100);
        assert_eq!(read_rax(&mut emu), 1);
        emu.vm.cpu.write_pc(B);
        emu.start(100);
        assert_eq!(read_rax(&mut emu), 3);
        assert!(group_at(&emu, B).unwrap().blocks.0 > b_group.blocks.0);
        let replacement = emu.vm.jit.entry_points[&B];
        for id in b_group.range() {
            emu.vm.jit.invalidate(id);
        }
        assert!(std::ptr::fn_addr_eq(emu.vm.jit.entry_points[&B], replacement));
    }

    #[test]
    fn manual_range_invalidates_group_crossing_page_boundary() {
        const PAGE: u64 = 0x30000;
        let entry = PAGE + 4094;
        let mut emu = IcicleEmulator::new_with_instruction_hooks(false);
        emu.range_invalidation_enabled = true;
        assert!(emu.map_memory(PAGE, 8192, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));
        assert!(emu.write_memory(entry, &code(7)));
        let group = emu.vm.lift(entry).expect("lift across boundary");
        assert!(emu.vm.cpu.mem.ensure_executable(PAGE + 4096, 1));
        assert!(group.range().any(|id| {
            let block = &emu.vm.code.blocks[id];
            block.start < PAGE + 4096 && block.end > PAGE + 4096
        }));
        assert!(emu.invalidate_code_range(PAGE + 4096, 1, MANUAL_PROTECT));
        emu.flush_pending_code();
        assert!(group_at(&emu, entry).is_none());
        assert!(group.range().all(|id| emu.vm.code.blocks[id].entry.is_none()));
        emu.vm.recompile();
        assert!(!emu.vm.jit.entry_points.contains_key(&entry));
        emu.vm.enable_jit = true;
        emu.vm.cpu.write_pc(entry);
        emu.start(100);
        assert_eq!(read_rax(&mut emu), 7);
        assert!(group_at(&emu, entry).unwrap().blocks.0 > group.blocks.0);
    }

    #[test]
    fn live_group_external_jump_reaches_replacement_after_recompile() {
        const A: u64 = 0x60000;
        const B: u64 = 0x70000;
        let mut emu = IcicleEmulator::new_with_instruction_hooks(false);
        emu.range_invalidation_enabled = true;
        emu.vm.enable_jit = true;
        for address in [A, B] {
            assert!(emu.map_memory(address, 4096, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));
        }
        let displacement = (B as i64 - (A as i64 + 5)) as i32;
        let mut jump = vec![0xe9];
        jump.extend_from_slice(&displacement.to_le_bytes());
        assert!(emu.write_memory(A, &jump));
        assert!(emu.write_memory(B, &code(2)));
        emu.vm.cpu.write_pc(A);
        emu.start(100);
        assert_eq!(read_rax(&mut emu), 2);
        let a_group = group_at(&emu, A).expect("live caller");
        let b_group = group_at(&emu, B).expect("old callee");
        assert!(emu.write_memory(B + 1, &3u32.to_le_bytes()));
        emu.flush_pending_code();
        assert_eq!(group_at(&emu, A).unwrap().blocks, a_group.blocks);
        assert!(group_at(&emu, B).is_none());
        assert!(b_group.range().all(|id| emu.vm.code.blocks[id].entry.is_none()));
        emu.vm.recompile();
        assert!(!emu.vm.jit.entry_points.contains_key(&B));
        emu.vm.cpu.write_pc(A);
        emu.start(100);
        assert_eq!(read_rax(&mut emu), 3);
        assert_eq!(group_at(&emu, A).unwrap().blocks, a_group.blocks);
        assert!(group_at(&emu, B).unwrap().blocks.0 > b_group.blocks.0);
    }

    #[test]
    fn merged_superblock_retires_both_entries_without_purging_live_groups() {
        const A: u64 = 0x80000;
        const B: u64 = 0x90000;
        let mut emu = IcicleEmulator::new_with_instruction_hooks(false);
        emu.range_invalidation_enabled = true;
        emu.vm.enable_jit = true;
        emu.vm.jit.profile_enabled = true;

        // Keep ten unrelated entrypoints alive. Retiring merged A/B must not cross
        // the JIT's dead-entry purge threshold, so this exercises the retained module.
        let clean: Vec<u64> = (0..10).map(|i| 0xa0000 + i * 0x10000).collect();
        for address in std::iter::once(A).chain(std::iter::once(B)).chain(clean.iter().copied()) {
            assert!(emu.map_memory(address, 4096, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));
        }
        let displacement = (B as i64 - (A as i64 + 5)) as i32;
        let mut jump = vec![0xe9];
        jump.extend_from_slice(&displacement.to_le_bytes());
        assert!(emu.write_memory(A, &jump));
        let mut b_loop = code(2);
        b_loop[6] = 0xf9; // jmp B, so the loop creates no separate B+5 entrypoint
        assert!(emu.write_memory(B, &b_loop));
        for (index, address) in clean.iter().copied().enumerate() {
            assert!(emu.write_memory(address, &code(10 + index as u32)));
        }
        for address in std::iter::once(A).chain(std::iter::once(B)).chain(clean.iter().copied()) {
            emu.vm.lift(address).expect("lift each independent group");
        }
        let a_group = group_at(&emu, A).unwrap();
        let old_b_group = group_at(&emu, B).unwrap();
        emu.vm.recompile();
        assert!(std::ptr::fn_addr_eq(emu.vm.jit.entry_points[&A], emu.vm.jit.entry_points[&B]),
            "upstream recompile must merge A->B for this regression");
        assert_eq!(emu.vm.jit.entry_points.len(), clean.len() + 2);
        let resets = emu.vm.jit.profile_reset_calls;
        emu.vm.cpu.write_pc(A);
        emu.start(100);
        assert_eq!(read_rax(&mut emu), 2);

        assert!(emu.write_memory(B + 1, &3u32.to_le_bytes()));
        emu.flush_pending_code();
        assert!(old_b_group.range().all(|id| emu.vm.code.blocks[id].entry.is_none()));
        assert!(!emu.vm.jit.entry_points.contains_key(&A), "merged caller must be retired");
        assert!(!emu.vm.jit.entry_points.contains_key(&B), "old callee must be retired");
        assert_eq!(group_at(&emu, A).unwrap().blocks, a_group.blocks);
        assert!(group_at(&emu, B).is_none());
        assert!(clean.iter().all(|address| emu.vm.jit.entry_points.contains_key(address)));
        assert!(!emu.vm.jit.should_purge());
        assert_eq!(emu.vm.jit.profile_reset_calls, resets);
        emu.vm.recompile();
        assert!(!emu.vm.jit.entry_points.contains_key(&B));
        assert_eq!(emu.vm.jit.profile_reset_calls, resets);
        emu.vm.cpu.write_pc(A);
        emu.start(100);
        assert_eq!(read_rax(&mut emu), 3);
        let replacement_b_group = group_at(&emu, B).unwrap();
        assert!(replacement_b_group.blocks.0 > old_b_group.blocks.0);
        assert_eq!(emu.vm.jit.profile_reset_calls, resets);

        // This recompile consumes the newly appended B group and advances
        // recompile_offset. A later replacement must still retire its current JIT
        // owner and must never resurrect either old B group.
        let compiles = emu.vm.jit.profile_compile_calls;
        emu.vm.recompile();
        assert!(emu.vm.jit.profile_compile_calls > compiles);
        assert!(emu.write_memory(B + 1, &4u32.to_le_bytes()));
        emu.flush_pending_code();
        assert!(replacement_b_group.range().all(|id| emu.vm.code.blocks[id].entry.is_none()));
        assert!(group_at(&emu, B).is_none());
        assert!(!emu.vm.jit.entry_points.contains_key(&B));
        assert!(!emu.vm.jit.should_purge());
        emu.vm.recompile();
        assert!(!emu.vm.jit.entry_points.contains_key(&B));
        assert_eq!(emu.vm.jit.profile_reset_calls, resets);
        emu.vm.cpu.write_pc(A);
        emu.start(100);
        assert_eq!(read_rax(&mut emu), 4);
        assert!(group_at(&emu, B).unwrap().blocks.0 > replacement_b_group.blocks.0);
        assert_eq!(emu.vm.jit.profile_reset_calls, resets);
    }

    #[test]
    fn epoch_or_wake_mixed_with_manual_range_uses_full_reset() {
        const A: u64 = 0x40000;
        const B: u64 = 0x50000;
        for reason in [INVALIDATION_EPOCH, INVALIDATION_WAKE] {
            let mut emu = IcicleEmulator::new_with_instruction_hooks(false);
            emu.range_invalidation_enabled = true;
            for address in [A, B] {
                assert!(emu.map_memory(address, 4096, FOREIGN_READ | FOREIGN_WRITE | FOREIGN_EXEC));
                assert!(emu.write_memory(address, &code(1)));
                emu.vm.lift(address).expect("lift");
            }
            assert!(emu.vm.cpu.mem.ensure_executable(A, 1));
            assert!(emu.invalidate_code_range(A, 1, MANUAL_PROTECT));
            emu.mark_invalidation_cause(reason);
            emu.flush_pending_code();
            assert!(emu.vm.code.map.is_empty());
            assert!(emu.pending_manual_pages.is_empty());
        }
    }
}
