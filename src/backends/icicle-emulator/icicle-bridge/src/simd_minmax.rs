use crate::aligned_move::{read_vector, write_vector};
use crate::vector_operand::{MemoryOperand, fault, reg};
use icicle_cpu::{Cpu, Exception, ExceptionCode, ValueSource};
use pcode::{Value, VarNode};

pub fn register(cpu: &mut Cpu) {
    cpu.set_helper(cpu.arch.sleigh.get_userop("sogen_minmax").unwrap(), execute);
}

fn extremum(
    mut first: u64,
    mut second: u64,
    element: usize,
    maximum: bool,
    daz: bool,
) -> (u64, u32) {
    let sign = 1u64 << (element * 8 - 1);
    let magnitude = sign - 1;
    let normal = if element == 4 {
        0x0080_0000
    } else {
        0x0010_0000_0000_0000
    };
    let infinity = magnitude & !(normal - 1);
    let denormal = |value: u64| (1..normal).contains(&(value & magnitude));
    if daz {
        if denormal(first) {
            first &= sign;
        }
        if denormal(second) {
            second &= sign;
        }
    }
    let a = first & magnitude;
    let b = second & magnitude;
    if a > infinity || b > infinity {
        return (second, 1);
    }
    let flags = if denormal(first) || denormal(second) {
        2
    } else {
        0
    };
    if a == 0 && b == 0 {
        return (second, flags);
    }
    let less = if (first ^ second) & sign != 0 {
        first & sign != 0
    } else if first & sign != 0 {
        first > second
    } else {
        first < second
    };
    let choose_first = if maximum {
        !less && first != second
    } else {
        less
    };
    (if choose_first { first } else { second }, flags)
}

fn lane_bits(bytes: &[u8]) -> u64 {
    let mut result = [0u8; 8];
    result[..bytes.len()].copy_from_slice(bytes);
    u64::from_le_bytes(result)
}

struct MinMax {
    family: u8,
    width: usize,
    element: usize,
    scalar: bool,
    maximum: bool,
    destination: usize,
    first: usize,
    second: usize,
    mask: u8,
    zero: bool,
    broadcast: bool,
    sae: bool,
    address: Option<u64>,
    stack: bool,
}

fn decode(cpu: &Cpu, bytes: &[u8], next: u64) -> Result<MinMax, Exception> {
    let invalid = || fault(cpu, 6);
    let mut offset = 0;
    let mut rex = 0;
    let mut forbidden = false;
    let mut locked = false;
    let mut legacy_pp = 0;
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
                legacy_pp = match bytes[offset] {
                    0x66 => 1,
                    0xf2 => 3,
                    _ => 2,
                };
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
    let (family, length, pp, r, b, x, high_r, high_b, v, mask, zero, broadcast_sae) =
        match bytes[offset] {
            0x0f if legacy_pp != 2 => {
                offset += 1;
                (
                    0,
                    0,
                    legacy_pp,
                    (rex >> 2) & 1,
                    rex & 1,
                    (rex >> 1) & 1,
                    0,
                    0,
                    0,
                    0,
                    false,
                    false,
                )
            }
            0xc5 if !forbidden && offset + 2 < bytes.len() => {
                let p = bytes[offset + 1];
                offset += 2;
                if p & 3 == 2 {
                    return Err(invalid());
                }
                (
                    1,
                    (p >> 2) & 1,
                    p & 3,
                    (!p >> 7) & 1,
                    0,
                    0,
                    0,
                    0,
                    (!p >> 3) & 15,
                    0,
                    false,
                    false,
                )
            }
            0xc4 if !forbidden && offset + 3 < bytes.len() => {
                let p0 = bytes[offset + 1];
                let p1 = bytes[offset + 2];
                offset += 3;
                if p0 & 31 != 1 || p1 & 3 == 2 {
                    return Err(invalid());
                }
                (
                    1,
                    (p1 >> 2) & 1,
                    p1 & 3,
                    (!p0 >> 7) & 1,
                    (!p0 >> 5) & 1,
                    (!p0 >> 6) & 1,
                    0,
                    0,
                    (!p1 >> 3) & 15,
                    0,
                    false,
                    false,
                )
            }
            0x62 if !forbidden && offset + 4 < bytes.len() => {
                let p0 = bytes[offset + 1];
                let p1 = bytes[offset + 2];
                let p2 = bytes[offset + 3];
                offset += 4;
                if p0 & 15 != 1
                    || p1 & 4 == 0
                    || p1 & 3 == 2
                    || (p1 >> 7 != u8::from(p1 & 3 != 0))
                    || p2 & 0x87 == 0x80
                {
                    return Err(invalid());
                }
                (
                    2,
                    (p2 >> 5) & 3,
                    p1 & 3,
                    (!p0 >> 7) & 1,
                    (!p0 >> 5) & 1,
                    (!p0 >> 6) & 1,
                    (!p0 >> 4) & 1,
                    (!p0 >> 6) & 1,
                    ((!p1 >> 3) & 15) | ((!p2 & 8) << 1),
                    p2 & 7,
                    p2 & 0x80 != 0,
                    p2 & 0x10 != 0,
                )
            }
            _ => return Err(invalid()),
        };
    let maximum = match bytes.get(offset) {
        Some(0x5d) => false,
        Some(0x5f) => true,
        _ => return Err(invalid()),
    };
    let scalar = pp == 3;
    let element = if pp == 0 { 4 } else { 8 };
    let modrm = *bytes.get(offset + 1).ok_or_else(invalid)?;
    offset += 2;
    let memory = modrm >> 6 != 3;
    let sae = family == 2 && broadcast_sae && !memory;
    if length == 3 && !sae && !scalar {
        return Err(invalid());
    }
    let width = if scalar {
        16
    } else if sae {
        64
    } else {
        16 << length
    };
    if scalar && memory && broadcast_sae {
        return Err(invalid());
    }
    let destination = ((modrm >> 3) & 7) as usize + 8 * r as usize + 16 * high_r as usize;
    let broadcast = broadcast_sae && memory;
    let (address, stack) = if memory {
        let operand = MemoryOperand {
            modrm,
            b,
            x,
            address32,
            segment,
            scale: if family == 2 {
                if broadcast || scalar { element } else { width }
            } else {
                1
            },
        };
        let (address, stack) = operand.address(cpu, bytes, offset, next)?;
        (Some(address), stack)
    } else {
        if offset != bytes.len() {
            return Err(invalid());
        }
        (None, false)
    };
    Ok(MinMax {
        family,
        width,
        element,
        scalar,
        maximum,
        destination,
        first: if family == 0 { destination } else { v as usize },
        second: (modrm & 7) as usize + 8 * b as usize + 16 * high_b as usize,
        mask,
        zero,
        broadcast,
        sae,
        address,
        stack,
    })
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
    let lanes = if op.scalar { 1 } else { op.width / op.element };
    let active = mask & ((1u64 << lanes) - 1);
    let first = read_vector(cpu, op.first);
    let mut second = read_vector(cpu, op.second);
    if let Some(address) = op.address {
        if op.family == 0 && !op.scalar && address & 15 != 0 {
            return Err(fault(cpu, 13));
        }
        let mut broadcast_value = None;
        for lane in 0..lanes {
            if active & (1 << lane) == 0 {
                continue;
            }
            let value = if let Some(value) = broadcast_value {
                value
            } else {
                let current =
                    address.wrapping_add(if op.broadcast { 0 } else { lane * op.element } as u64);
                let canonical = |v: u64| (((v as i64) << 16) >> 16) as u64 == v;
                if !canonical(current)
                    || !current
                        .checked_add(op.element as u64 - 1)
                        .is_some_and(canonical)
                {
                    return Err(fault(cpu, if op.stack { 12 } else { 13 }));
                }
                let mut value = [0u8; 8];
                for byte in 0..op.element {
                    value[byte] = cpu
                        .mem
                        .read::<1>(current + byte as u64, icicle_cpu::mem::perm::READ)
                        .map_err(|e| {
                            Exception::new(ExceptionCode::from_load_error(e), current + byte as u64)
                        })?[0];
                }
                if (op.broadcast || op.scalar)
                    && current & (op.element as u64 - 1) != 0
                    && cr0 & (1 << 18) != 0
                    && cpu.read_var::<u8>(reg(cpu, "AC")) != 0
                    && cpu.read_var::<u16>(reg(cpu, "CS")) & 3 == 3
                {
                    return Err(fault(cpu, 17));
                }
                if op.broadcast {
                    broadcast_value = Some(value);
                }
                value
            };
            second[lane * op.element..(lane + 1) * op.element]
                .copy_from_slice(&value[..op.element]);
        }
    }
    let control = reg(cpu, "MXCSR");
    let mxcsr = cpu.read_var::<u32>(control);
    let mut output = read_vector(cpu, op.destination);
    if op.scalar && op.family != 0 {
        output[8..16].copy_from_slice(&first[8..16]);
    }
    let mut exceptions = 0;
    for lane in 0..lanes {
        let range = lane * op.element..(lane + 1) * op.element;
        if active & (1 << lane) == 0 {
            if op.zero {
                output[range].fill(0);
            }
            continue;
        }
        let (value, flags) = extremum(
            lane_bits(&first[range.clone()]),
            lane_bits(&second[range.clone()]),
            op.element,
            op.maximum,
            mxcsr & 0x40 != 0,
        );
        exceptions |= flags;
        output[range].copy_from_slice(&value.to_le_bytes()[..op.element]);
    }
    if !op.sae {
        cpu.write_var(control, mxcsr | exceptions);
        if exceptions & !(mxcsr >> 7) != 0 {
            return Err(fault(cpu, if cr4 & 0x400 != 0 { 19 } else { 6 }));
        }
    }
    if op.family != 0 {
        output[op.width..].fill(0);
    }
    write_vector(cpu, op.destination, &output);
    Ok(())
}

fn execute(cpu: &mut Cpu, _: VarNode, args: [Value; 2]) {
    if let Err(exception) = run(cpu, cpu.read::<u64>(args[0])) {
        cpu.exception = exception;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(target_arch = "x86_64")]
    fn hardware(first: u64, second: u64, element: usize, maximum: bool, mxcsr: u32) -> (u64, u32) {
        use std::arch::{asm, x86_64::__m128i};
        let mut saved = 0u32;
        let mut flags = 0u32;
        let control = mxcsr | 0x1f80;
        let a = if element == 4 {
            (first as u128) * 0x00000001000000010000000100000001
        } else {
            (first as u128) * 0x10000000000000001
        };
        let b = if element == 4 {
            (second as u128) * 0x00000001000000010000000100000001
        } else {
            (second as u128) * 0x10000000000000001
        };
        unsafe {
            let mut result: __m128i = std::mem::transmute(a);
            let source: __m128i = std::mem::transmute(b);
            macro_rules! compare {
                ($instruction:literal) => {
                    asm!("stmxcsr [{saved}]", "ldmxcsr [{control}]", $instruction,
                        "stmxcsr [{flags}]", "ldmxcsr [{saved}]", saved = in(reg) &mut saved,
                        control = in(reg) &control, flags = in(reg) &mut flags,
                        result = inout(xmm_reg) result, source = in(xmm_reg) source, options(nostack, preserves_flags))
                }
            }
            match (element, maximum) {
                (4, false) => compare!("minps {result}, {source}"),
                (4, true) => compare!("maxps {result}, {source}"),
                (8, false) => compare!("minpd {result}, {source}"),
                (8, true) => compare!("maxpd {result}, {source}"),
                _ => unreachable!(),
            }
            let bytes: [u8; 16] = std::mem::transmute(result);
            (lane_bits(&bytes[..element]), flags)
        }
    }

    #[cfg(target_arch = "x86_64")]
    #[test]
    fn minimum_and_maximum_results_and_flags_match_hardware() {
        for element in [4, 8] {
            let sign = 1u64 << (element * 8 - 1);
            let normal = if element == 4 {
                0x800000
            } else {
                0x10000000000000
            };
            let infinity = (sign - 1) & !(normal - 1);
            let values = [
                0,
                sign,
                1,
                normal - 1,
                sign | 1,
                sign | (normal - 1),
                normal,
                sign | normal,
                infinity - 1,
                infinity | sign,
                infinity,
                infinity | (normal / 2) | 12345,
                infinity | 12345,
                infinity | sign | (normal / 2) | 54321,
                infinity | sign | 12345,
            ];
            for maximum in [false, true] {
                for control in [0x1f80, 0x1fc0, 0x9f80, 0x9fc0] {
                    for first in values {
                        for second in values {
                            let (value, flags) =
                                extremum(first, second, element, maximum, control & 0x40 != 0);
                            assert_eq!(
                                (value, control | flags),
                                hardware(first, second, element, maximum, control),
                                "first={first:016x} second={second:016x} element={element} maximum={maximum} mxcsr={control:04x}"
                            );
                        }
                    }
                }
            }
        }
    }
}
