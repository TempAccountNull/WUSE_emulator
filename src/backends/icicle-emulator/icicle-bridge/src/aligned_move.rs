use crate::registers::X86Register;
use icicle_cpu::{Cpu, Exception, ExceptionCode, ValueSource};
use pcode::{Value, VarNode};
use std::sync::OnceLock;

static VECTORS: OnceLock<[[VarNode; 4]; 32]> = OnceLock::new();

fn reg(cpu: &Cpu, name: &str) -> VarNode {
    cpu.arch.sleigh.get_reg(name).unwrap().get_raw_var()
}

pub fn register(cpu: &mut Cpu) {
    for index in 0..32 {
        for part in if index < 16 { 2 } else { 0 }..4 {
            cpu.arch
                .sleigh
                .add_custom_reg(&format!("ZMM{index}_PART{part}"), 16)
                .unwrap();
        }
    }
    for index in 0..8 {
        cpu.arch
            .sleigh
            .add_custom_reg(&format!("K{index}"), 8)
            .unwrap();
    }
    let vectors = std::array::from_fn(|index| {
        std::array::from_fn(|part| vector_part_lookup(cpu, index, part))
    });
    assert_eq!(*VECTORS.get_or_init(|| vectors), vectors);
    cpu.set_helper(
        cpu.arch.sleigh.get_userop("sogen_aligned_move").unwrap(),
        execute,
    );
}

fn vector_part(_: &Cpu, index: usize, part: usize) -> VarNode {
    VECTORS.get().unwrap()[index][part]
}

fn vector_part_lookup(cpu: &Cpu, index: usize, part: usize) -> VarNode {
    if index < 16 && part < 2 {
        cpu.arch
            .sleigh
            .get_reg(&format!("YMM{index}"))
            .unwrap()
            .slice_var((part * 16) as u8, 16)
            .unwrap()
    } else {
        reg(cpu, &format!("ZMM{index}_PART{part}"))
    }
}

pub fn read_vector(cpu: &Cpu, index: usize) -> [u8; 64] {
    let mut result = [0; 64];
    for part in 0..4 {
        result[part * 16..part * 16 + 16]
            .copy_from_slice(&cpu.read_var::<[u8; 16]>(vector_part(cpu, index, part)));
    }
    result
}

pub fn write_vector(cpu: &mut Cpu, index: usize, value: &[u8; 64]) {
    for part in 0..4 {
        cpu.write_var(
            vector_part(cpu, index, part),
            <[u8; 16]>::try_from(&value[part * 16..part * 16 + 16]).unwrap(),
        );
    }
}

fn register_range(value: i32, first: X86Register, count: usize) -> Option<usize> {
    let offset = value - first as i32;
    (offset >= 0 && (offset as usize) < count).then_some(offset as usize)
}

fn vector_register(value: &X86Register) -> Option<(usize, usize)> {
    let value = *value as i32;
    for (first, size) in [
        (X86Register::Xmm0, 16),
        (X86Register::Ymm0, 32),
        (X86Register::Zmm0, 64),
    ] {
        if let Some(index) = register_range(value, first, 32) {
            return Some((index, size));
        }
    }
    None
}

pub fn read_register(cpu: &Cpu, value: &X86Register, data: &mut [u8]) -> Option<usize> {
    if let Some((index, size)) = vector_register(value) {
        let bytes = read_vector(cpu, index);
        let count = size.min(data.len());
        data[..count].copy_from_slice(&bytes[..count]);
        return Some(size);
    }
    let value = *value as i32;
    if let Some(index) = register_range(value, X86Register::K0, 8) {
        let bytes = cpu
            .read_var::<u64>(reg(cpu, &format!("K{index}")))
            .to_le_bytes();
        let count = 8.min(data.len());
        data[..count].copy_from_slice(&bytes[..count]);
        return Some(8);
    }
    None
}

pub fn write_register(cpu: &mut Cpu, value: &X86Register, data: &[u8]) -> Option<usize> {
    if let Some((index, size)) = vector_register(value) {
        let mut bytes = read_vector(cpu, index);
        bytes[..size].fill(0);
        let count = size.min(data.len());
        bytes[..count].copy_from_slice(&data[..count]);
        write_vector(cpu, index, &bytes);
        return Some(size);
    }
    let value = *value as i32;
    if let Some(index) = register_range(value, X86Register::K0, 8) {
        let mut bytes = [0; 8];
        let count = 8.min(data.len());
        bytes[..count].copy_from_slice(&data[..count]);
        cpu.write_var(reg(cpu, &format!("K{index}")), u64::from_le_bytes(bytes));
        return Some(8);
    }
    None
}

fn fault(cpu: &Cpu, vector: u64) -> Exception {
    if vector == 6 {
        (ExceptionCode::InvalidInstruction, cpu.read_pc()).into()
    } else {
        (
            ExceptionCode::Environment,
            crate::packed_sad::ARCHITECTURAL_FAULT | vector,
        )
            .into()
    }
}

#[derive(Debug)]
struct Move {
    family: u8,
    width: usize,
    element: usize,
    mask: u8,
    zero: bool,
    store: bool,
    vector: usize,
    other: usize,
    address: Option<u64>,
    stack: bool,
}

fn decode(cpu: &Cpu, bytes: &[u8], next: u64) -> Result<Move, Exception> {
    let invalid = || fault(cpu, 6);
    let mut offset = 0;
    let mut rex = 0;
    let mut forbidden = false;
    let mut locked = false;
    let mut address32 = false;
    let mut segment = 0;
    while offset < bytes.len() {
        match bytes[offset] {
            0x40..=0x4f => {
                rex = bytes[offset];
                forbidden = true;
            }
            0x66 | 0xf2 | 0xf3 => {
                rex = 0;
                forbidden = true;
            }
            0xf0 => {
                locked = true;
                rex = 0;
            }
            0x67 => {
                address32 = true;
                rex = 0;
            }
            0x26 | 0x2e | 0x36 | 0x3e | 0x64 | 0x65 => {
                segment = bytes[offset];
                rex = 0;
            }
            _ => break,
        }
        offset += 1;
    }
    if locked || offset >= bytes.len() {
        return Err(invalid());
    }
    let (family, width, element, mask, zero, r, b, x, high_r, high_b) = match bytes[offset] {
        0x0f => {
            offset += 1;
            (
                0,
                16,
                16,
                0,
                false,
                (rex >> 2) & 1,
                rex & 1,
                (rex >> 1) & 1,
                0,
                0,
            )
        }
        0xc5 if offset + 2 < bytes.len() && !forbidden => {
            let p = bytes[offset + 1];
            offset += 2;
            if p & 0x7b != 0x79 {
                return Err(invalid());
            }
            (
                1,
                16 << ((p >> 2) & 1),
                16,
                0,
                false,
                (!p >> 7) & 1,
                0,
                0,
                0,
                0,
            )
        }
        0xc4 if offset + 3 < bytes.len() && !forbidden => {
            let p0 = bytes[offset + 1];
            let p1 = bytes[offset + 2];
            offset += 3;
            if p0 & 31 != 1 || p1 & 0x7b != 0x79 {
                return Err(invalid());
            }
            (
                1,
                16 << ((p1 >> 2) & 1),
                16,
                0,
                false,
                (!p0 >> 7) & 1,
                (!p0 >> 5) & 1,
                (!p0 >> 6) & 1,
                0,
                0,
            )
        }
        0x62 if offset + 4 < bytes.len() && !forbidden => {
            let p0 = bytes[offset + 1];
            let p1 = bytes[offset + 2];
            let p2 = bytes[offset + 3];
            offset += 4;
            if p0 & 15 != 1
                || p1 & 0x7f != 0x7d
                || p2 & 0x18 != 8
                || p2 & 0x60 == 0x60
                || p2 & 0x87 == 0x80
            {
                return Err(invalid());
            }
            (
                2,
                16 << ((p2 >> 5) & 3),
                4 << (p1 >> 7),
                p2 & 7,
                p2 & 0x80 != 0,
                (!p0 >> 7) & 1,
                (!p0 >> 5) & 1,
                (!p0 >> 6) & 1,
                (!p0 >> 4) & 1,
                (!p0 >> 6) & 1,
            )
        }
        _ => return Err(invalid()),
    };
    if offset + 2 > bytes.len() {
        return Err(invalid());
    }
    let opcode = bytes[offset];
    let modrm = bytes[offset + 1];
    offset += 2;
    if opcode != 0x6f && opcode != 0x7f {
        return Err(invalid());
    }
    let mode = modrm >> 6;
    let rm = modrm & 7;
    let mut result = Move {
        family,
        width,
        element,
        mask,
        zero,
        store: opcode == 0x7f,
        vector: ((modrm >> 3) & 7) as usize + 8 * r as usize + 16 * high_r as usize,
        other: rm as usize + 8 * b as usize + 16 * high_b as usize,
        address: None,
        stack: false,
    };
    if mode == 3 {
        return if offset == bytes.len() {
            Ok(result)
        } else {
            Err(invalid())
        };
    }
    if result.store && zero {
        return Err(invalid());
    }
    let (address, stack) = crate::vector_operand::MemoryOperand {
        modrm,
        b,
        x,
        address32,
        segment,
        scale: if family == 2 { width } else { 1 },
    }
    .address(cpu, bytes, offset, next)?;
    result.stack = stack;
    result.address = Some(address);
    Ok(result)
}

fn run(cpu: &mut Cpu, next: u64) -> Result<(), Exception> {
    let start = cpu.read_pc();
    let length = next
        .checked_sub(start)
        .filter(|n| *n > 0 && *n <= 15)
        .ok_or_else(|| fault(cpu, 6))? as usize;
    let mut bytes = [0; 15];
    cpu.mem
        .read_bytes(start, &mut bytes[..length], icicle_cpu::mem::perm::NONE)
        .map_err(|error| Exception::new(ExceptionCode::from_load_error(error), start))?;
    let op = decode(cpu, &bytes[..length], next)?;
    let cr0 = cpu.read_var::<u64>(reg(cpu, "CR0"));
    let cr4 = cpu.read_var::<u64>(reg(cpu, "CR4"));
    let xcr0 = cpu.read_var::<u64>(reg(cpu, "XCR0"));
    let enabled = match op.family {
        0 => cr0 & 4 == 0 && cr4 & 0x200 != 0,
        1 => cr4 & 0x40000 != 0 && xcr0 & 6 == 6,
        _ => cr4 & 0x40000 != 0 && xcr0 & 0xe7 == 0xe7,
    };
    if !enabled {
        return Err(fault(cpu, 6));
    }
    if cr0 & 8 != 0 {
        return Err(fault(cpu, 7));
    }
    let mask = if op.mask == 0 {
        u64::MAX
    } else {
        cpu.read_var::<u64>(reg(cpu, &format!("K{}", op.mask)))
    };
    let lanes = op.width / op.element;
    let active = mask & ((1u64 << lanes) - 1);
    if let Some(address) = op.address {
        // Type E1 does not suppress the alignment check, even for an all-zero writemask.
        if address & (op.width as u64 - 1) != 0 {
            return Err(fault(cpu, 13));
        }
        if active != 0 && ((((address as i64) << 16) >> 16) as u64 != address) {
            return Err(fault(cpu, if op.stack { 12 } else { 13 }));
        }
    }
    let destination = if op.store && op.address.is_none() {
        op.other
    } else {
        op.vector
    };
    let source = if op.store { op.vector } else { op.other };
    let source_bytes = read_vector(cpu, source);
    let mut output = read_vector(cpu, destination);
    for lane in 0..lanes {
        let offset = lane * op.element;
        if active & (1 << lane) == 0 {
            if op.zero {
                output[offset..offset + op.element].fill(0);
            }
            continue;
        }
        if let Some(address) = op.address {
            if op.store {
                for part in (0..op.element).step_by(4) {
                    let current = address + (offset + part) as u64;
                    cpu.mem
                        .write::<4>(
                            current,
                            source_bytes[offset + part..offset + part + 4]
                                .try_into()
                                .unwrap(),
                            icicle_cpu::mem::perm::WRITE,
                        )
                        .map_err(|error| {
                            Exception::new(ExceptionCode::from_store_error(error), current)
                        })?;
                }
            } else {
                for part in (0..op.element).step_by(4) {
                    let current = address + (offset + part) as u64;
                    let value = cpu
                        .mem
                        .read::<4>(current, icicle_cpu::mem::perm::READ)
                        .map_err(|error| {
                            Exception::new(ExceptionCode::from_load_error(error), current)
                        })?;
                    output[offset + part..offset + part + 4].copy_from_slice(&value);
                }
            }
        } else {
            output[offset..offset + op.element]
                .copy_from_slice(&source_bytes[offset..offset + op.element]);
        }
    }
    if !op.store || op.address.is_none() {
        if op.family != 0 {
            output[op.width..].fill(0);
        }
        write_vector(cpu, destination, &output);
    }
    Ok(())
}

fn execute(cpu: &mut Cpu, _: VarNode, args: [Value; 2]) {
    let next = cpu.read::<u64>(args[0]);
    if let Err(exception) = run(cpu, next) {
        cpu.exception = exception;
    }
}
