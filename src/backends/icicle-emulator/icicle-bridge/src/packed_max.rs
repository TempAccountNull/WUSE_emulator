use icicle_cpu::{Cpu, ExceptionCode, ValueSource};
use pcode::{Value, VarNode};

pub const SIMD_EXCEPTION: u64 = 0x53494d44;

pub fn register(cpu: &mut Cpu) {
    cpu.set_helper(cpu.arch.sleigh.get_userop("maxps").unwrap(), packed);
    cpu.set_helper(
        cpu.arch.sleigh.get_userop("sogen_maxps_memory").unwrap(),
        memory,
    );
}

fn denormal(value: u32) -> bool {
    (1..0x0080_0000).contains(&(value & 0x7fff_ffff))
}

fn lane(mut first: u32, mut second: u32, daz: bool) -> (u32, u32) {
    if daz {
        if denormal(first) {
            first &= 0x8000_0000;
        }
        if denormal(second) {
            second &= 0x8000_0000;
        }
    }
    let a = first & 0x7fff_ffff;
    let b = second & 0x7fff_ffff;
    if a > 0x7f80_0000 || b > 0x7f80_0000 {
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
    let greater = if (first ^ second) & 0x8000_0000 != 0 {
        first & 0x8000_0000 == 0
    } else if first & 0x8000_0000 != 0 {
        first < second
    } else {
        first > second
    };
    (if greater { first } else { second }, flags)
}

fn maximum(first: u128, second: u128, mxcsr: u32) -> (u128, u32) {
    let mut result = 0;
    let mut exceptions = 0;
    for index in 0..4 {
        let shift = index * 32;
        let (value, flags) = lane(
            (first >> shift) as u32,
            (second >> shift) as u32,
            mxcsr & 0x40 != 0,
        );
        result |= (value as u128) << shift;
        exceptions |= flags;
    }
    (result, exceptions)
}

fn memory(cpu: &mut Cpu, dst: VarNode, args: [Value; 2]) {
    let address = cpu.read::<u64>(args[1]);
    if address & 15 != 0 {
        cpu.exception = (
            ExceptionCode::Environment,
            crate::xstate::GENERAL_PROTECTION,
        )
            .into();
        return;
    }
    match cpu.mem.read::<16>(address, icicle_cpu::mem::perm::READ) {
        Ok(bytes) => complete(
            cpu,
            dst,
            cpu.read::<u128>(args[0]),
            u128::from_le_bytes(bytes),
        ),
        Err(error) => cpu.exception = (ExceptionCode::from_load_error(error), address).into(),
    }
}

fn packed(cpu: &mut Cpu, dst: VarNode, args: [Value; 2]) {
    complete(
        cpu,
        dst,
        cpu.read::<u128>(args[0]),
        cpu.read::<u128>(args[1]),
    );
}

fn complete(cpu: &mut Cpu, dst: VarNode, first: u128, second: u128) {
    let control = cpu.arch.sleigh.get_reg("MXCSR").unwrap().get_raw_var();
    let mxcsr = cpu.read_var::<u32>(control);
    let (result, flags) = maximum(first, second, mxcsr);
    cpu.write_var(control, mxcsr | flags);
    if flags & !(mxcsr >> 7) != 0 {
        cpu.exception = (ExceptionCode::Environment, SIMD_EXCEPTION).into();
        return;
    }
    cpu.write_var(dst, result);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn second_operand_selection_and_denormals() {
        for (first, second, daz, result, flags) in [
            (0, 0x8000_0000, false, 0x8000_0000, 0),
            (0x8000_0000, 0, false, 0, 0),
            (0x7fc1_2345, 0x3f80_0000, false, 0x3f80_0000, 1),
            (0x3f80_0000, 0x7f81_2345, false, 0x7f81_2345, 1),
            (0x7f81_2345, 0xffc5_4321, false, 0xffc5_4321, 1),
            (1, 0, false, 1, 2),
            (0x8000_0001, 0x8000_0002, false, 0x8000_0001, 2),
            (1, 0x8000_0001, true, 0x8000_0000, 0),
            (0xbf80_0000, 0xc000_0000, false, 0xbf80_0000, 0),
        ] {
            assert_eq!(lane(first, second, daz), (result, flags));
        }
    }

    #[cfg(target_arch = "x86_64")]
    fn hardware(first: u128, second: u128, mxcsr: u32) -> (u128, u32) {
        use std::arch::{asm, x86_64::__m128};
        let mut saved = 0u32;
        let mut flags = 0u32;
        let control = mxcsr | 0x1f80;
        unsafe {
            let mut result: __m128 = std::mem::transmute(first);
            let source: __m128 = std::mem::transmute(second);
            asm!(
                "stmxcsr [{saved}]",
                "ldmxcsr [{control}]",
                "maxps {result}, {source}",
                "stmxcsr [{flags}]",
                "ldmxcsr [{saved}]",
                saved = in(reg) &mut saved,
                control = in(reg) &control,
                flags = in(reg) &mut flags,
                result = inout(xmm_reg) result,
                source = in(xmm_reg) source,
                options(nostack, preserves_flags),
            );
            (std::mem::transmute(result), flags)
        }
    }

    #[cfg(target_arch = "x86_64")]
    #[test]
    fn masked_results_and_flags_match_hardware() {
        let values: [u32; 20] = [
            0,
            0x8000_0000,
            1,
            0x007f_ffff,
            0x8000_0001,
            0x807f_ffff,
            0x0080_0000,
            0x8080_0000,
            0x3f80_0000,
            0xbf80_0000,
            0x3f7f_fffe,
            0xbf7f_fffe,
            0x7f7f_ffff,
            0xff7f_ffff,
            0x7f80_0000,
            0xff80_0000,
            0x7fc1_2345,
            0xffc5_4321,
            0x7f81_2345,
            0xff81_2345,
        ];
        for mxcsr in [0x1f80, 0x1fc0, 0x9f80, 0x9fc0] {
            for &first in &values {
                for &second in &values {
                    let a = (first as u128) * 0x00000001000000010000000100000001;
                    let b = (second as u128) * 0x00000001000000010000000100000001;
                    let (result, flags) = maximum(a, b, mxcsr);
                    assert_eq!(
                        (result, mxcsr | flags),
                        hardware(a, b, mxcsr),
                        "first={first:08x} second={second:08x} mxcsr={mxcsr:04x}"
                    );
                }
            }
            let mut state = 0x12345678abcdeffedcba9876543210u128;
            for _ in 0..2000 {
                state = state
                    .wrapping_mul(6364136223846793005)
                    .wrapping_add(1442695040888963407);
                let first = state;
                state = state
                    .wrapping_mul(6364136223846793005)
                    .wrapping_add(1442695040888963407);
                let (result, flags) = maximum(first, state, mxcsr);
                assert_eq!((result, mxcsr | flags), hardware(first, state, mxcsr));
            }
        }
    }
}
