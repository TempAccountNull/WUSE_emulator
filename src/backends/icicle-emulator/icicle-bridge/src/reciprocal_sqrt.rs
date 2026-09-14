use icicle_cpu::{Cpu, ValueSource};
use pcode::{Value, VarNode};

pub fn register(cpu: &mut Cpu) {
    cpu.set_helper(cpu.arch.sleigh.get_userop("rsqrtps").unwrap(), packed);
    cpu.set_helper(cpu.arch.sleigh.get_userop("rsqrtss").unwrap(), scalar);
}

#[cfg(target_arch = "x86_64")]
fn estimate(source: u128) -> u128 {
    use std::arch::x86_64::{__m128, _mm_rsqrt_ps};
    // RSQRT's approximation is implementation-dependent; use the host x86 estimate.
    unsafe { std::mem::transmute(_mm_rsqrt_ps(std::mem::transmute::<u128, __m128>(source))) }
}

#[cfg(not(target_arch = "x86_64"))]
fn estimate(source: u128) -> u128 {
    let mut result = 0u128;
    for lane in 0..4 {
        result |= (portable_lane((source >> (lane * 32)) as u32) as u128) << (lane * 32);
    }
    result
}

#[cfg(any(test, not(target_arch = "x86_64")))]
fn portable_lane(source: u32) -> u32 {
    let magnitude = source & 0x7fff_ffff;
    if magnitude > 0x7f80_0000 {
        source | 0x0040_0000
    } else if magnitude < 0x0080_0000 {
        (source & 0x8000_0000) | 0x7f80_0000
    } else if source & 0x8000_0000 != 0 {
        0xffc0_0000
    } else if magnitude == 0x7f80_0000 {
        0
    } else {
        (1.0 / (f32::from_bits(source) as f64).sqrt() as f32).to_bits()
    }
}

fn packed(cpu: &mut Cpu, dst: VarNode, args: [Value; 2]) {
    let source = cpu.read::<u128>(args[1]);
    cpu.write_var(dst, estimate(source));
}

fn scalar(cpu: &mut Cpu, dst: VarNode, args: [Value; 2]) {
    let original = cpu.read::<u128>(args[0]);
    let source = cpu.read::<u32>(args[1].slice(0, 4));
    let result = estimate(source as u128) & 0xffff_ffff;
    cpu.write_var(dst, (original & !0xffff_ffffu128) | result);
}

#[cfg(test)]
mod tests {
    use super::portable_lane;

    #[test]
    fn portable_special_values() {
        for (source, expected) in [
            (0, 0x7f80_0000),
            (0x8000_0000, 0xff80_0000),
            (1, 0x7f80_0000),
            (0x807f_ffff, 0xff80_0000),
            (0xbf80_0000, 0xffc0_0000),
            (0xff80_0000, 0xffc0_0000),
            (0x7f80_0000, 0),
            (0x7f81_2345, 0x7fc1_2345),
            (0xffc5_4321, 0xffc5_4321),
        ] {
            assert_eq!(portable_lane(source), expected);
        }
    }

    #[test]
    fn portable_error_bound_across_normal_exponents() {
        for exponent in 1..255 {
            for fraction in [0, 1, 0x123456, 0x3fffff, 0x7fffff] {
                let source = (exponent << 23) | fraction;
                let value = f32::from_bits(source) as f64;
                let result = f32::from_bits(portable_lane(source)) as f64;
                assert!((result * value.sqrt() - 1.0).abs() <= 1.5 / 4096.0);
            }
        }
    }
}
