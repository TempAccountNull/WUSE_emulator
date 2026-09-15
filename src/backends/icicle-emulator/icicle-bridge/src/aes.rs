use icicle_cpu::{Cpu, ExceptionCode, ValueSource};
use pcode::{Value, VarNode};

pub fn register(cpu: &mut Cpu) {
    cpu.set_helper(
        cpu.arch.sleigh.get_userop("aeskeygenassist").unwrap(),
        keygen,
    );
    cpu.set_helper(cpu.arch.sleigh.get_userop("aesimc").unwrap(), inverse_mix);
    cpu.set_helper(
        cpu.arch.sleigh.get_userop("aesenc").unwrap(),
        round::<false, false>,
    );
    cpu.set_helper(
        cpu.arch.sleigh.get_userop("aesenclast").unwrap(),
        round::<false, true>,
    );
    cpu.set_helper(
        cpu.arch.sleigh.get_userop("aesdec").unwrap(),
        round::<true, false>,
    );
    cpu.set_helper(
        cpu.arch.sleigh.get_userop("aesdeclast").unwrap(),
        round::<true, true>,
    );
    cpu.set_helper(
        cpu.arch.sleigh.get_userop("sogen_aes_memory").unwrap(),
        memory,
    );
}

const fn multiply(mut value: u8, mut factor: u8) -> u8 {
    let mut result = 0;
    while factor != 0 {
        if factor & 1 != 0 {
            result ^= value;
        }
        value = (value << 1) ^ if value & 0x80 != 0 { 0x1b } else { 0 };
        factor >>= 1;
    }
    result
}

const fn substitution_tables() -> [[u8; 256]; 2] {
    let mut tables = [[0; 256]; 2];
    let mut index = 0;
    while index < 256 {
        let mut inverse = 1;
        let mut power = index as u8;
        let mut exponent = 254;
        while exponent != 0 {
            if exponent & 1 != 0 {
                inverse = multiply(inverse, power);
            }
            power = multiply(power, power);
            exponent >>= 1;
        }
        let value = inverse
            ^ inverse.rotate_left(1)
            ^ inverse.rotate_left(2)
            ^ inverse.rotate_left(3)
            ^ inverse.rotate_left(4)
            ^ 0x63;
        tables[0][index] = value;
        tables[1][value as usize] = index as u8;
        index += 1;
    }
    tables
}

const SUBSTITUTIONS: [[u8; 256]; 2] = substitution_tables();

fn assist(source: u128, constant: u8) -> u128 {
    let mut result = 0;
    for (input, output) in [(32, 0), (96, 64)] {
        let word = (source >> input) as u32;
        let substituted =
            u32::from_le_bytes(word.to_le_bytes().map(|v| SUBSTITUTIONS[0][v as usize]));
        result |= (substituted as u128) << output;
        result |= ((substituted.rotate_right(8) ^ constant as u32) as u128) << (output + 32);
    }
    result
}

fn mix_columns<const INVERSE: bool>(source: u128) -> u128 {
    let bytes = source.to_le_bytes();
    let factors = if INVERSE {
        [14, 11, 13, 9]
    } else {
        [2, 3, 1, 1]
    };
    let mut result = [0; 16];
    for column in 0..4 {
        for row in 0..4 {
            for offset in 0..4 {
                result[4 * column + row] ^=
                    multiply(bytes[4 * column + (row + offset) % 4], factors[offset]);
            }
        }
    }
    u128::from_le_bytes(result)
}

fn transform<const INVERSE: bool, const LAST: bool>(source: u128, key: u128) -> u128 {
    let bytes = source.to_le_bytes();
    let mut result = [0; 16];
    for column in 0..4 {
        for row in 0..4 {
            let shifted = (column + if INVERSE { 4 - row } else { row }) % 4;
            result[4 * column + row] =
                SUBSTITUTIONS[INVERSE as usize][bytes[4 * shifted + row] as usize];
        }
    }
    let state = u128::from_le_bytes(result);
    (if LAST {
        state
    } else {
        mix_columns::<INVERSE>(state)
    }) ^ key
}

fn keygen(cpu: &mut Cpu, dst: VarNode, args: [Value; 2]) {
    let result = assist(cpu.read::<u128>(args[0]), cpu.read::<u8>(args[1]));
    cpu.write_var(dst, result);
}

fn inverse_mix(cpu: &mut Cpu, dst: VarNode, args: [Value; 2]) {
    let result = mix_columns::<true>(cpu.read::<u128>(args[0]));
    cpu.write_var(dst, result);
}

fn round<const INVERSE: bool, const LAST: bool>(cpu: &mut Cpu, dst: VarNode, args: [Value; 2]) {
    let result = transform::<INVERSE, LAST>(cpu.read::<u128>(args[0]), cpu.read::<u128>(args[1]));
    cpu.write_var(dst, result);
}

fn memory(cpu: &mut Cpu, dst: VarNode, args: [Value; 2]) {
    let address = cpu.read::<u64>(args[0]);
    if address & 15 != 0 {
        cpu.exception = (
            ExceptionCode::Environment,
            crate::xstate::GENERAL_PROTECTION,
        )
            .into();
        return;
    }
    match cpu.mem.read::<16>(address, icicle_cpu::mem::perm::READ) {
        Ok(bytes) => cpu.write_var(dst, u128::from_le_bytes(bytes)),
        Err(error) => cpu.exception = (ExceptionCode::from_load_error(error), address).into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn captured_zero_key_and_all_round_constants() {
        for constant in 0..=255u8 {
            let expected = 0x63636363_63636363_63636363_63636363u128
                ^ ((constant as u128) << 32)
                ^ ((constant as u128) << 96);
            assert_eq!(assist(0, constant), expected);
        }
    }

    #[test]
    fn fips_197_first_encryption_round() {
        let state = 0x00102030405060708090a0b0c0d0e0f0u128.swap_bytes();
        let key = 0xd6aa74fdd2af72fadaa678f1d6ab76feu128.swap_bytes();
        let expected = 0x89d810e8855ace682d1843d8cb128fe4u128.swap_bytes();
        assert_eq!(transform::<false, false>(state, key), expected);
        assert_eq!(mix_columns::<true>(mix_columns::<false>(state)), state);
    }

    #[cfg(target_arch = "x86_64")]
    #[test]
    fn round_operations_match_hardware() {
        use std::arch::x86_64::*;
        if !is_x86_feature_detected!("aes") {
            return;
        }
        let mut source = 0x123456789abcdef0123456789abcdef0u128;
        for _ in 0..1024 {
            source ^= source << 13;
            source ^= source >> 7;
            source ^= source << 17;
            let key = source.rotate_left(53) ^ 0x15a537691f358cd817a689900182835fu128;
            unsafe {
                let a: __m128i = std::mem::transmute(source);
                let b: __m128i = std::mem::transmute(key);
                assert_eq!(
                    transform::<false, false>(source, key),
                    std::mem::transmute::<_, u128>(_mm_aesenc_si128(a, b))
                );
                assert_eq!(
                    transform::<false, true>(source, key),
                    std::mem::transmute::<_, u128>(_mm_aesenclast_si128(a, b))
                );
                assert_eq!(
                    transform::<true, false>(source, key),
                    std::mem::transmute::<_, u128>(_mm_aesdec_si128(a, b))
                );
                assert_eq!(
                    transform::<true, true>(source, key),
                    std::mem::transmute::<_, u128>(_mm_aesdeclast_si128(a, b))
                );
                assert_eq!(
                    mix_columns::<true>(source),
                    std::mem::transmute::<_, u128>(_mm_aesimc_si128(a))
                );
            }
        }
    }

    #[cfg(target_arch = "x86_64")]
    #[test]
    fn key_assist_matches_hardware_for_every_substitution_byte() {
        use std::arch::x86_64::*;
        if !is_x86_feature_detected!("aes") {
            return;
        }
        for byte in 0..=255u8 {
            let source =
                u128::from_le_bytes(std::array::from_fn(|i| byte.wrapping_add(i as u8 * 13)));
            unsafe {
                let a: __m128i = std::mem::transmute(source);
                macro_rules! check {
                    ($($constant:literal),*) => { $(
                        assert_eq!(assist(source, $constant), std::mem::transmute::<_, u128>(_mm_aeskeygenassist_si128::<$constant>(a)));
                    )* };
                }
                check!(0, 1, 2, 4, 8, 16, 32, 64, 128, 27, 54, 255);
            }
        }
    }
}
