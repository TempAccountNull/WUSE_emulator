use icicle_cpu::ExceptionCode;
use icicle_cpu::ValueSource;
use std::cell::Cell;
use std::collections::{BTreeMap, HashSet};
use std::time::Instant;
use std::{cell::RefCell, collections::HashMap, rc::Rc};

use crate::registers;

fn create_x64_vm() -> icicle_vm::Vm {
    let mut cpu_config = icicle_vm::cpu::Config::from_target_triple("x86_64-none");
    cpu_config.enable_jit = std::env::var("SOGEN_ICICLE_JIT").as_deref() == Ok("1");
    cpu_config.enable_jit_mem = true;
    cpu_config.enable_shadow_stack = false;
    cpu_config.enable_recompilation = true;
    cpu_config.track_uninitialized = false;
    cpu_config.optimize_instructions = true;
    cpu_config.optimize_block = false;

    let mut vm = icicle_vm::build(&cpu_config).unwrap();
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
                    if is_first_inst {
                        is_first_inst = false;
                        tmp_block.push((pcode::Op::Arg(0), pcode::Inputs::one(inst_count)));
                        tmp_block.push(pcode::Op::Hook(self.block_hook));
                    }

                    tmp_block.push(pcode::Op::Hook(self.inst_hook));
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

struct ExecutionHooks {
    stop: Rc<RefCell<bool>>,
    invalidate_code: Rc<Cell<bool>>,
    generic_hooks: HookContainer<dyn Fn(u64)>,
    ranged_hooks: HookContainer<dyn Fn(u64)>,
    specific_hooks: HookContainer<dyn Fn(u64)>,
    block_hooks: HookContainer<dyn Fn(u64, u64)>,
    address_mapping: BTreeMap<u64, Vec<u32>>,
    address_filter: [u64; 4],
    one_time_callbacks: Vec<Box<dyn Fn()>>,
}

impl ExecutionHooks {
    pub fn new(stop_value: Rc<RefCell<bool>>, invalidate_code: Rc<Cell<bool>>) -> Self {
        Self {
            stop: stop_value,
            invalidate_code,
            generic_hooks: HookContainer::new(),
            ranged_hooks: HookContainer::new(),
            specific_hooks: HookContainer::new(),
            block_hooks: HookContainer::new(),
            address_mapping: BTreeMap::new(),
            address_filter: [0; 4],
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

    pub fn execute(&mut self, cpu: &mut icicle_cpu::Cpu, address: u64) {
        if self.invalidate_code.get() {
            cpu.exception =
                icicle_cpu::Exception::new(ExceptionCode::Environment, CACHE_INVALIDATED);
            return;
        }
        self.run_hooks(address);

        if *self.stop.borrow() {
            cpu.exception.code = ExceptionCode::InstructionLimit as u32;
            cpu.exception.value = address;
        } else if self.invalidate_code.get() {
            cpu.exception =
                icicle_cpu::Exception::new(ExceptionCode::Environment, CACHE_INVALIDATED);
        }
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
        let mut virtual_machine = create_x64_vm();
        let capacity_400mb = 50_000;

        let mut capacity = 8 * 2 * capacity_400mb; // ~8gb
        if cfg!(target_pointer_width = "32") {
            capacity = 2 * capacity_400mb; // ~1gb
        }

        virtual_machine.cpu.mem.set_capacity(capacity);

        let stop_value = Rc::new(RefCell::new(false));
        let invalidate_code = Rc::new(Cell::new(false));
        let exec_hooks = Rc::new(RefCell::new(ExecutionHooks::new(
            stop_value.clone(),
            invalidate_code.clone(),
        )));

        let inst_exec_hooks = Rc::clone(&exec_hooks);

        let inst_hook = icicle_cpu::InstHook::new(move |cpu: &mut icicle_cpu::Cpu, addr: u64| {
            inst_exec_hooks.borrow_mut().execute(cpu, addr);
        });

        let block_exec_hooks = Rc::clone(&exec_hooks);

        let block_hook = icicle_cpu::InstHook::new(move |cpu: &mut icicle_cpu::Cpu, addr: u64| {
            let instructions = cpu.args[0] as u64;
            block_exec_hooks.borrow_mut().on_block(addr, instructions);
        });

        let inst_hook_id = virtual_machine.cpu.add_hook(inst_hook);
        let block_hook_id = virtual_machine.cpu.add_hook(block_hook);
        virtual_machine.add_injector(InstructionHookInjector {
            inst_hook: inst_hook_id,
            block_hook: block_hook_id,
        });

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

    pub fn start(&mut self, count: u64) {
        self.executing_thread = std::thread::current().id();
        self.last_stop = IcicleStopInfo::none();
        self.last_vm_exit = icicle_vm::VmExit::Running;

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

            self.vm_running = true;
            let reason = self.vm.run();
            self.vm_running = false;
            self.last_vm_exit = reason;

            match reason {
                icicle_vm::VmExit::InstructionLimit => {
                    self.last_stop = IcicleStopInfo::instruction_limit();
                    break;
                }
                icicle_vm::VmExit::UnhandledException((code, value)) => {
                    let continue_execution = self.handle_exception(code, value);
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

    fn invalidate_code_range(&mut self, address: u64, length: u64) -> bool {
        if length == 0 {
            return false;
        }
        let last = address.saturating_add(length - 1);
        let page_size = self.vm.cpu.mem.page_size();
        let mut page_address = self.vm.cpu.mem.page_aligned(address);
        let mut changed = false;
        loop {
            if let Some(index) = self.vm.cpu.mem.get_physical_index(page_address) {
                let page = self.vm.cpu.mem.get_physical_mut(index);
                if page.executed {
                    page.executed = false;
                    for permission in &mut page.data_mut().perm {
                        *permission &= !icicle_cpu::mem::perm::IN_CODE_CACHE;
                    }
                    changed = true;
                }
            }
            if last - page_address < page_size {
                break;
            }
            page_address += page_size;
        }
        if changed {
            self.vm.cpu.mem.clear_tlb();
            self.invalidate_code.set(true);
        }
        changed
    }

    fn flush_pending_code(&mut self) {
        if self.invalidate_code.replace(false) {
            assert!(!self.vm_running);
            self.vm.code.flush_code();
            // Vm::run has returned, so no generated-code frame still references this module.
            unsafe { self.vm.jit.reset() };
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
        if !self.invalidate_code_range(address, length) {
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
            self.flush_pending_code();
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

        self.syscall_hooks.for_each_hook(|func| {
            func();
        });

        self.vm.cpu.write_pc(self.vm.cpu.read_pc() + 2);
        return true;
    }

    pub fn stop(&mut self) {
        self.vm.icount_limit = 0;

        if self.executing_thread == std::thread::current().id() {
            *self.stop.borrow_mut() = true;
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
            self.invalidate_code_range(address, size);
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
        self.invalidate_code_range(address, length);
        let mem = &mut self.vm.cpu.mem;
        for (_, _, entry) in mem.mapping.overlapping_iter(address..=last) {
            if let Some(icicle_cpu::mem::MemoryMapping::Physical(page)) = entry {
                if !page.index.is_zero_page() {
                    self.pending_free_pages.push(page.index);
                }
            }
        }
        let result = mem.unmap_memory_len(address, length);
        self.reclaim_pending_pages();
        result
    }

    pub fn protect_memory(&mut self, address: u64, length: u64, permissions: u8) -> bool {
        self.invalidate_code_range(address, length);
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
        self.invalidate_code_range(address, data.len() as u64);
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
        match mem.write_bytes(address, data, icicle_vm::cpu::mem::perm::NONE) {
            Ok(()) => true,
            Err(error) => {
                eprintln!(
                    "Icicle memory write failed: address={address:#x} bytes={} error={error:?} physical_pages={} capacity={}",
                    data.len(),
                    mem.total_pages(),
                    mem.capacity()
                );
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
