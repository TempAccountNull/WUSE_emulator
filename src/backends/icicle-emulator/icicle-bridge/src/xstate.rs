use icicle_cpu::{Cpu, Exception, ExceptionCode, ValueSource};
use pcode::{Value, VarNode};
use std::sync::OnceLock;

const FEATURES: u64 = 3;
pub const GENERAL_PROTECTION: u64 = 0x5853544750;
type Helper = fn(&mut Cpu, VarNode, [Value; 2]);
static CPUID: OnceLock<Helper> = OnceLock::new();
static CPUID_BASIC: OnceLock<Helper> = OnceLock::new();
static CPUID_VERSION: OnceLock<Helper> = OnceLock::new();

fn reg(cpu: &Cpu, name: &str) -> VarNode {
    cpu.arch.sleigh.get_reg(name).unwrap().get_raw_var()
}

pub fn register(cpu: &mut Cpu) {
    for (name, helper) in [
        ("xrstor", restore as Helper),
        ("xrstor64", restore64 as Helper),
        ("xsave", save as Helper),
        ("xsave64", save64 as Helper),
    ] {
        cpu.set_helper(cpu.arch.sleigh.get_userop(name).unwrap(), helper);
    }
    let cpuid = cpu.arch.sleigh.get_userop("cpuid").unwrap();
    CPUID.get_or_init(|| cpu.helpers[cpuid as usize]);
    cpu.set_helper(cpuid, query);
    for name in [
        "cpuid_Processor_Extended_States_info",
        "cpuid_cache_tlb_info",
        "cpuid_serial_info",
        "cpuid_Deterministic_Cache_Parameters_info",
        "cpuid_MONITOR_MWAIT_Features_info",
        "cpuid_Thermal_Power_Management_info",
        "cpuid_Direct_Cache_Access_info",
        "cpuid_Architectural_Performance_Monitoring_info",
        "cpuid_Extended_Topology_info",
    ] {
        cpu.set_helper(cpu.arch.sleigh.get_userop(name).unwrap(), query);
    }
    let version = cpu.arch.sleigh.get_userop("cpuid_Version_info").unwrap();
    CPUID_VERSION.get_or_init(|| cpu.helpers[version as usize]);
    cpu.set_helper(version, version_query);
    let basic = cpu.arch.sleigh.get_userop("cpuid_basic_info").unwrap();
    CPUID_BASIC.get_or_init(|| cpu.helpers[basic as usize]);
    cpu.set_helper(basic, basic_query);
    let xcr0 = reg(cpu, "XCR0");
    cpu.arch.reg_init.push((xcr0, FEATURES as u128));
    cpu.write_var(xcr0, FEATURES);
}

pub fn restore_legacy_xcr0(cpu: &mut Cpu) {
    let xcr0 = reg(cpu, "XCR0");
    // Older snapshots left XCR0 at zero; bit zero must be set in a valid Windows CPU state.
    if cpu.read_var::<u64>(xcr0) == 0 {
        cpu.write_var(xcr0, FEATURES);
    }
}

fn basic_query(cpu: &mut Cpu, dst: VarNode, args: [Value; 2]) {
    CPUID_BASIC.get().unwrap()(cpu, dst, args);
    cpu.write_var(dst.slice(0, 4), 0xdu32);
}

fn version_query(cpu: &mut Cpu, dst: VarNode, args: [Value; 2]) {
    CPUID_VERSION.get().unwrap()(cpu, dst, args);
    // The SLEIGH result order is EAX, EBX, EDX, ECX; Icicle's version helper swaps the last pair.
    let ecx = cpu.read_var::<u32>(dst.slice(8, 4));
    let edx = cpu.read_var::<u32>(dst.slice(12, 4));
    cpu.write_var(dst.slice(8, 4), edx);
    cpu.write_var(dst.slice(12, 4), ecx | (1 << 27));
}

fn query(cpu: &mut Cpu, dst: VarNode, args: [Value; 2]) {
    let leaf = cpu.read::<u32>(args[0]);
    if leaf > 0xd {
        CPUID.get().unwrap()(cpu, dst, args);
        return;
    }
    let values = if leaf == 0xd && cpu.read::<u32>(args[1]) == 0 {
        [FEATURES as u32, 576, 0, 576]
    } else {
        [0; 4]
    };
    for (index, value) in values.into_iter().enumerate() {
        cpu.write_var(dst.slice((index * 4) as u8, 4), value);
    }
}

fn gp() -> Exception {
    (ExceptionCode::Environment, GENERAL_PROTECTION).into()
}

fn load(cpu: &mut Cpu, address: u64, bytes: usize) -> Result<Vec<u8>, Exception> {
    let mut result = Vec::with_capacity(bytes);
    for offset in 0..bytes {
        let current = address.checked_add(offset as u64).ok_or_else(gp)?;
        let value = cpu
            .mem
            .read::<1>(current, icicle_cpu::mem::perm::READ)
            .map_err(|error| Exception::new(ExceptionCode::from_load_error(error), current))?;
        result.push(value[0]);
    }
    Ok(result)
}

fn store(cpu: &mut Cpu, address: u64, data: &[u8]) -> Result<(), Exception> {
    for (offset, value) in data.iter().enumerate() {
        let current = address.checked_add(offset as u64).ok_or_else(gp)?;
        cpu.mem
            .write::<1>(current, [*value], icicle_cpu::mem::perm::WRITE)
            .map_err(|error| Exception::new(ExceptionCode::from_store_error(error), current))?;
    }
    Ok(())
}

fn read_register(cpu: &Cpu, name: &str) -> Vec<u8> {
    let var = reg(cpu, name);
    (0..var.size)
        .map(|index| cpu.read_var::<u8>(var.slice(index, 1)))
        .collect()
}

fn write_register(cpu: &mut Cpu, name: &str, value: &[u8]) {
    let var = reg(cpu, name);
    for index in 0..var.size {
        cpu.write_var(
            var.slice(index, 1),
            value.get(index as usize).copied().unwrap_or(0),
        );
    }
}

fn mask(cpu: &Cpu) -> u64 {
    let requested = u64::from(cpu.read_var::<u32>(reg(cpu, "EAX")))
        | (u64::from(cpu.read_var::<u32>(reg(cpu, "EDX"))) << 32);
    requested & cpu.read_var::<u64>(reg(cpu, "XCR0"))
}

fn validate_address(address: u64) -> Result<(), Exception> {
    if address & 63 != 0 || (((address as i64) << 16) >> 16) as u64 != address {
        return Err(gp());
    }
    Ok(())
}

fn restore(cpu: &mut Cpu, _: VarNode, args: [Value; 2]) {
    do_restore(cpu, args, false);
}
fn restore64(cpu: &mut Cpu, _: VarNode, args: [Value; 2]) {
    do_restore(cpu, args, true);
}
fn save(cpu: &mut Cpu, _: VarNode, args: [Value; 2]) {
    do_save(cpu, args, false);
}
fn save64(cpu: &mut Cpu, _: VarNode, args: [Value; 2]) {
    do_save(cpu, args, true);
}

fn do_restore(cpu: &mut Cpu, args: [Value; 2], wide: bool) {
    let address = cpu.read::<u64>(args[0]);
    if let Err(exception) = restore_state(cpu, address, wide) {
        cpu.exception = exception;
    }
}

fn restore_state(cpu: &mut Cpu, address: u64, wide: bool) -> Result<(), Exception> {
    validate_address(address)?;
    let header = load(cpu, address + 512, 24)?;
    let present = u64::from_le_bytes(header[..8].try_into().unwrap());
    let enabled = cpu.read_var::<u64>(reg(cpu, "XCR0"));
    if present & !enabled != 0
        || enabled & !FEATURES != 0
        || header[8..].iter().any(|value| *value != 0)
    {
        return Err(gp());
    }
    let requested = mask(cpu);
    let mut fields: Vec<(String, Vec<u8>)> = Vec::new();
    if requested & 1 != 0 {
        let active = present & 1 != 0;
        let data = if active {
            load(cpu, address, 24)?
        } else {
            let mut data = vec![0; 24];
            data[..2].copy_from_slice(&0x037fu16.to_le_bytes());
            data
        };
        for (name, offset, size) in [
            ("FPUControlWord", 0, 2),
            ("FPUStatusWord", 2, 2),
            ("FPULastInstructionOpcode", 6, 2),
            ("FPUInstructionPointer", 8, if wide { 8 } else { 4 }),
            ("FPUDataPointer", 16, if wide { 8 } else { 4 }),
        ] {
            fields.push((name.into(), data[offset..offset + size].to_vec()));
        }
        for (name, offset) in [("FPUPointerSelector", 12), ("FPUDataSelector", 20)] {
            fields.push((
                name.into(),
                if wide {
                    vec![0; 2]
                } else {
                    data[offset..offset + 2].to_vec()
                },
            ));
        }
        let status = u16::from_le_bytes(data[2..4].try_into().unwrap());
        let top = (status >> 11) & 7;
        let mut tags = 0u16;
        for index in 0..8 {
            let value = if active {
                load(cpu, address + 32 + index * 16, 10)?
            } else {
                vec![0; 10]
            };
            let physical = (index as u16 + top) & 7;
            let fraction = u64::from_le_bytes(value[..8].try_into().unwrap());
            let exponent = u16::from_le_bytes(value[8..].try_into().unwrap()) & 0x7fff;
            let tag = if data[4] & (1 << physical) == 0 {
                3
            } else if exponent == 0 && fraction == 0 {
                1
            } else if exponent == 0 || exponent == 0x7fff || fraction >> 63 == 0 {
                2
            } else {
                0
            };
            tags |= tag << (physical * 2);
            fields.push((format!("ST{index}"), value));
        }
        fields.push(("FPUTagWord".into(), tags.to_le_bytes().to_vec()));
        for (name, bit) in [("C0", 8), ("C1", 9), ("C2", 10), ("C3", 14)] {
            fields.push((name.into(), vec![((status >> bit) & 1) as u8]));
        }
    }
    if requested & 2 != 0 {
        let mxcsr = load(cpu, address + 24, 4)?;
        if u32::from_le_bytes(mxcsr.as_slice().try_into().unwrap()) & !0xffff != 0 {
            return Err(gp());
        }
        fields.push(("MXCSR".into(), mxcsr));
        for index in 0..16 {
            let value = if present & 2 != 0 {
                load(cpu, address + 160 + index * 16, 16)?
            } else {
                vec![0; 16]
            };
            fields.push((format!("XMM{index}"), value));
        }
    }
    for (name, value) in fields {
        write_register(cpu, &name, &value);
    }
    Ok(())
}

fn do_save(cpu: &mut Cpu, args: [Value; 2], wide: bool) {
    let address = cpu.read::<u64>(args[0]);
    if let Err(exception) = save_state(cpu, address, wide) {
        cpu.exception = exception;
    }
}

fn save_state(cpu: &mut Cpu, address: u64, wide: bool) -> Result<(), Exception> {
    validate_address(address)?;
    let requested = mask(cpu);
    if requested & !FEATURES != 0 {
        return Err(gp());
    }
    let old = u64::from_le_bytes(load(cpu, address + 512, 8)?.try_into().unwrap());
    if requested & 1 != 0 {
        for (name, offset) in [
            ("FPUControlWord", 0),
            ("FPUStatusWord", 2),
            ("FPULastInstructionOpcode", 6),
        ] {
            store(cpu, address + offset, &read_register(cpu, name))?;
        }
        let tags = cpu.read_var::<u16>(reg(cpu, "FPUTagWord"));
        let abridged = (0..8).fold(0u8, |value, index| {
            value | (u8::from((tags >> (index * 2)) & 3 != 3) << index)
        });
        store(cpu, address + 4, &[abridged])?;
        for (name, offset) in [("FPUInstructionPointer", 8), ("FPUDataPointer", 16)] {
            let value = read_register(cpu, name);
            store(cpu, address + offset, &value[..if wide { 8 } else { 4 }])?;
        }
        if !wide {
            store(cpu, address + 12, &read_register(cpu, "FPUPointerSelector"))?;
            store(cpu, address + 20, &read_register(cpu, "FPUDataSelector"))?;
        }
        for index in 0..8 {
            store(
                cpu,
                address + 32 + index * 16,
                &read_register(cpu, &format!("ST{index}")),
            )?;
        }
    }
    if requested & 2 != 0 {
        store(cpu, address + 24, &read_register(cpu, "MXCSR"))?;
        store(cpu, address + 28, &0xffffu32.to_le_bytes())?;
        for index in 0..16 {
            store(
                cpu,
                address + 160 + index * 16,
                &read_register(cpu, &format!("XMM{index}")),
            )?;
        }
    }
    // A processor may conservatively track a component as in use; XSAVE still preserves unrequested bits.
    store(
        cpu,
        address + 512,
        &((old & !requested) | requested).to_le_bytes(),
    )
}
