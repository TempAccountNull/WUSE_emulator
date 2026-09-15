use crate::aligned_move::{read_vector, write_vector};
use crate::vector_operand::{MemoryOperand, fault, reg};
use icicle_cpu::{Cpu, Exception, ExceptionCode, ValueSource};
use pcode::{Value, VarNode};

pub fn register(cpu: &mut Cpu) {
    cpu.set_helper(
        cpu.arch.sleigh.get_userop("sogen_pmulhuw").unwrap(),
        execute,
    );
}

#[derive(Clone, Copy, PartialEq)]
enum Family {
    Mmx,
    Sse,
    Vex,
    Evex,
}

struct Multiply {
    family: Family,
    width: usize,
    destination: usize,
    first: usize,
    second: usize,
    mask: u8,
    zero: bool,
    address: Option<u64>,
    stack: bool,
}

fn decode(cpu: &Cpu, bytes: &[u8], next: u64) -> Result<Multiply, Exception> {
    let invalid = || fault(cpu, 6);
    let mut offset = 0;
    let mut rex = 0;
    let mut forbidden = false;
    let mut locked = false;
    let mut pp = 0;
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
                pp = if bytes[offset] == 0x66 { 1 } else { 2 };
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
    let (family, width, r, b, x, high_r, high_b, v, mask, zero) = match bytes[offset] {
        0x0f if pp < 2 => {
            offset += 1;
            (
                if pp == 0 { Family::Mmx } else { Family::Sse },
                if pp == 0 { 8 } else { 16 },
                (rex >> 2) & 1,
                rex & 1,
                (rex >> 1) & 1,
                0,
                0,
                0,
                0,
                false,
            )
        }
        0xc5 if !forbidden && offset + 2 < bytes.len() => {
            let p = bytes[offset + 1];
            offset += 2;
            if p & 3 != 1 {
                return Err(invalid());
            }
            (
                Family::Vex,
                16 << ((p >> 2) & 1),
                (!p >> 7) & 1,
                0,
                0,
                0,
                0,
                (!p >> 3) & 15,
                0,
                false,
            )
        }
        0xc4 if !forbidden && offset + 3 < bytes.len() => {
            let p0 = bytes[offset + 1];
            let p1 = bytes[offset + 2];
            offset += 3;
            if p0 & 31 != 1 || p1 & 3 != 1 {
                return Err(invalid());
            }
            (
                Family::Vex,
                16 << ((p1 >> 2) & 1),
                (!p0 >> 7) & 1,
                (!p0 >> 5) & 1,
                (!p0 >> 6) & 1,
                0,
                0,
                (!p1 >> 3) & 15,
                0,
                false,
            )
        }
        0x62 if !forbidden && offset + 4 < bytes.len() => {
            let p0 = bytes[offset + 1];
            let p1 = bytes[offset + 2];
            let p2 = bytes[offset + 3];
            offset += 4;
            if p0 & 15 != 1
                || p1 & 7 != 5
                || p2 & 0x10 != 0
                || p2 & 0x60 == 0x60
                || p2 & 0x87 == 0x80
            {
                return Err(invalid());
            }
            (
                Family::Evex,
                16 << ((p2 >> 5) & 3),
                (!p0 >> 7) & 1,
                (!p0 >> 5) & 1,
                (!p0 >> 6) & 1,
                (!p0 >> 4) & 1,
                (!p0 >> 6) & 1,
                ((!p1 >> 3) & 15) | ((!p2 & 8) << 1),
                p2 & 7,
                p2 & 0x80 != 0,
            )
        }
        _ => return Err(invalid()),
    };
    if bytes.get(offset) != Some(&0xe4) {
        return Err(invalid());
    }
    let modrm = *bytes.get(offset + 1).ok_or_else(invalid)?;
    offset += 2;
    let destination = ((modrm >> 3) & 7) as usize
        + if family == Family::Mmx {
            0
        } else {
            8 * r as usize + 16 * high_r as usize
        };
    let second = (modrm & 7) as usize
        + if family == Family::Mmx {
            0
        } else {
            8 * b as usize + 16 * high_b as usize
        };
    let (address, stack) = if modrm >> 6 != 3 {
        let operand = MemoryOperand {
            modrm,
            b,
            x,
            address32,
            segment,
            scale: if family == Family::Evex { width } else { 1 },
        };
        let (address, stack) = operand.address(cpu, bytes, offset, next)?;
        (Some(address), stack)
    } else {
        if offset != bytes.len() {
            return Err(invalid());
        }
        (None, false)
    };
    Ok(Multiply {
        family,
        width,
        destination,
        second,
        first: if matches!(family, Family::Mmx | Family::Sse) {
            destination
        } else {
            v as usize
        },
        mask,
        zero,
        address,
        stack,
    })
}

fn product(first: u16, second: u16) -> u16 {
    ((u32::from(first) * u32::from(second)) >> 16) as u16
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
        .map_err(|e| Exception::new(ExceptionCode::from_load_error(e), start))?;
    let op = decode(cpu, &bytes[..length], next)?;
    let cr0 = cpu.read_var::<u64>(reg(cpu, "CR0"));
    let cr4 = cpu.read_var::<u64>(reg(cpu, "CR4"));
    let xcr0 = cpu.read_var::<u64>(reg(cpu, "XCR0"));
    let enabled = match op.family {
        Family::Mmx => cr0 & 4 == 0,
        Family::Sse => cr0 & 4 == 0 && cr4 & 0x200 != 0,
        Family::Vex => cr4 & 0x40000 != 0 && xcr0 & 6 == 6,
        Family::Evex => cr4 & 0x40000 != 0 && xcr0 & 0xe7 == 0xe7,
    };
    if !enabled {
        return Err(fault(cpu, 6));
    }
    if cr0 & 8 != 0 {
        return Err(fault(cpu, 7));
    }
    let mmx = op.family == Family::Mmx;
    let status = cpu.read_var::<u16>(reg(cpu, "FPUStatusWord"));
    if mmx && status & !cpu.read_var::<u16>(reg(cpu, "FPUControlWord")) & 0x3f != 0 {
        return Err(fault(cpu, 16));
    }
    let mask = if op.mask == 0 {
        u64::MAX
    } else {
        cpu.read_var::<u64>(reg(cpu, &format!("K{}", op.mask)))
    };
    let lanes = op.width / 2;
    let top = (status >> 11) as usize & 7;
    let physical = |cpu: &Cpu, index: usize| -> [u8; 10] {
        // SLEIGH stores logical ST registers; MMX addresses physical x87 registers.
        cpu.read_var(reg(cpu, &format!("ST{}", (index + 8 - top) & 7)))
    };
    let read_operand = |cpu: &Cpu, index: usize| {
        if mmx {
            let mut bytes = [0; 64];
            bytes[..8].copy_from_slice(&physical(cpu, index)[..8]);
            bytes
        } else {
            read_vector(cpu, index)
        }
    };
    let first = read_operand(cpu, op.first);
    let mut second = read_operand(cpu, op.second);
    if let Some(address) = op.address {
        let canonical = |v: u64| (((v as i64) << 16) >> 16) as u64 == v;
        if op.family != Family::Evex
            && (!canonical(address)
                || !address
                    .checked_add(op.width as u64 - 1)
                    .is_some_and(canonical))
        {
            return Err(fault(cpu, if op.stack { 12 } else { 13 }));
        }
        if op.family == Family::Sse && address & 15 != 0 {
            return Err(fault(cpu, 13));
        }
        for lane in 0..lanes {
            if mask & (1 << lane) == 0 {
                continue;
            }
            let current = address.wrapping_add((lane * 2) as u64);
            if !canonical(current) || !current.checked_add(1).is_some_and(canonical) {
                return Err(fault(cpu, if op.stack { 12 } else { 13 }));
            }
            for byte in 0..2 {
                second[lane * 2 + byte] = cpu
                    .mem
                    .read::<1>(current + byte as u64, icicle_cpu::mem::perm::READ)
                    .map_err(|e| {
                        Exception::new(ExceptionCode::from_load_error(e), current + byte as u64)
                    })?[0];
            }
        }
        let alignment = if mmx {
            8
        } else if op.family == Family::Evex {
            2
        } else {
            1
        };
        if mask & ((1u64 << lanes) - 1) != 0
            && address & (alignment - 1) != 0
            && cr0 & (1 << 18) != 0
            && cpu.read_var::<u8>(reg(cpu, "AC")) != 0
            && cpu.read_var::<u16>(reg(cpu, "CS")) & 3 == 3
        {
            return Err(fault(cpu, 17));
        }
    }
    let mut output = read_operand(cpu, op.destination);
    for lane in 0..lanes {
        let range = lane * 2..lane * 2 + 2;
        if mask & (1 << lane) == 0 {
            if op.zero {
                output[range].fill(0);
            }
            continue;
        }
        let left = u16::from_le_bytes(first[range.clone()].try_into().unwrap());
        let right = u16::from_le_bytes(second[range.clone()].try_into().unwrap());
        output[range].copy_from_slice(&product(left, right).to_le_bytes());
    }
    if mmx {
        let mut registers: [[u8; 10]; 8] = std::array::from_fn(|index| physical(cpu, index));
        registers[op.destination][..8].copy_from_slice(&output[..8]);
        registers[op.destination][8..].fill(0xff);
        for (index, value) in registers.into_iter().enumerate() {
            cpu.write_var(reg(cpu, &format!("ST{index}")), value);
        }
        cpu.write_var(reg(cpu, "FPUTagWord"), 0u16);
        cpu.write_var(reg(cpu, "FPUStatusWord"), status & !0x3800);
    } else {
        if op.family != Family::Sse {
            output[op.width..].fill(0);
        }
        write_vector(cpu, op.destination, &output);
    }
    Ok(())
}

fn execute(cpu: &mut Cpu, _: VarNode, args: [Value; 2]) {
    if let Err(exception) = run(cpu, cpu.read::<u64>(args[0])) {
        cpu.exception = exception;
    }
}

#[cfg(test)]
mod tests {
    use super::product;

    #[test]
    fn every_word_matches_full_width_reference_at_boundaries() {
        for first in 0..=u16::MAX {
            for second in [0, 1, 2, 127, 255, 256, 32767, 32768, 65534, 65535] {
                assert_eq!(
                    product(first, second),
                    (u64::from(first) * u64::from(second) / 65536) as u16
                );
            }
        }
    }

    #[cfg(target_arch = "x86_64")]
    #[test]
    fn mixed_vectors_match_sse2_hardware() {
        use std::arch::x86_64::{__m128i, _mm_mulhi_epu16};
        let mut seed = 0x123456789abcdef013579bdf2468ace0u128;
        for _ in 0..4096 {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            let other = seed.rotate_left(43) ^ 0xffff00018000ffff7fff80000001ffff;
            let first: [u16; 8] = unsafe { std::mem::transmute(seed) };
            let second: [u16; 8] = unsafe { std::mem::transmute(other) };
            let actual = std::array::from_fn::<u16, 8, _>(|i| product(first[i], second[i]));
            let expected: [u16; 8] = unsafe {
                std::mem::transmute(_mm_mulhi_epu16(
                    std::mem::transmute::<u128, __m128i>(seed),
                    std::mem::transmute::<u128, __m128i>(other),
                ))
            };
            assert_eq!(actual, expected);
        }
    }
}
