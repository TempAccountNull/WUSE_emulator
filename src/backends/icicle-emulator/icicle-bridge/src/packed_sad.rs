use icicle_cpu::{Cpu, ExceptionCode, ValueSource};
use pcode::{Value, VarNode};

pub fn register(cpu: &mut Cpu) {
    for name in ["psadbw", "vpsadbw_avx", "vpsadbw_avx2"] {
        cpu.set_helper(cpu.arch.sleigh.get_userop(name).unwrap(), packed);
    }
    cpu.set_helper(
        cpu.arch.sleigh.get_userop("sogen_sad_memory").unwrap(),
        memory,
    );
    cpu.set_helper(
        cpu.arch.sleigh.get_userop("sogen_sad_mmx_enter").unwrap(),
        enter_mmx,
    );
    cpu.set_helper(
        cpu.arch.sleigh.get_userop("sogen_sad_check").unwrap(),
        check,
    );
    for (name, value) in [("CR0", 0x80010033u64), ("CR4", 0x40620u64)] {
        let node = reg(cpu, name);
        cpu.arch.reg_init.push((node, value as u128));
        cpu.write_var(node, value);
    }
}

fn sum<const N: usize>(left: [u8; N], right: [u8; N]) -> [u8; N] {
    let mut output = [0; N];
    for lane in 0..N / 8 {
        let start = lane * 8;
        let value: u16 = (start..start + 8)
            .map(|i| left[i].abs_diff(right[i]) as u16)
            .sum();
        output[start..start + 2].copy_from_slice(&value.to_le_bytes());
    }
    output
}

fn packed(cpu: &mut Cpu, dst: VarNode, args: [Value; 2]) {
    match dst.size {
        8 => {
            let left = cpu.read::<u64>(args[0]).to_le_bytes();
            let right = cpu.read::<u64>(args[1]).to_le_bytes();
            cpu.write_var(dst, u64::from_le_bytes(sum(left, right)));
            for index in 0..8 {
                let mm = cpu
                    .arch
                    .sleigh
                    .get_reg(&format!("MM{index}"))
                    .unwrap()
                    .get_raw_var();
                if dst == mm {
                    let st = cpu
                        .arch
                        .sleigh
                        .get_reg(&format!("ST{index}"))
                        .unwrap()
                        .get_raw_var();
                    cpu.write_var(st.slice(8, 2), 0xffffu16);
                    break;
                }
            }
            let tag = cpu.arch.sleigh.get_reg("FPUTagWord").unwrap().get_raw_var();
            let status = cpu
                .arch
                .sleigh
                .get_reg("FPUStatusWord")
                .unwrap()
                .get_raw_var();
            cpu.write_var(tag, 0u16);
            cpu.write_var(status, cpu.read_var::<u16>(status) & !0x3800);
        }
        16 => {
            let left = cpu.read::<u128>(args[0]).to_le_bytes();
            let right = cpu.read::<u128>(args[1]).to_le_bytes();
            cpu.write_var(dst, u128::from_le_bytes(sum(left, right)));
        }
        32 => {
            let left: [u8; 32] = cpu.read_dynamic(args[0]).zxt();
            let right: [u8; 32] = cpu.read_dynamic(args[1]).zxt();
            cpu.write_var(dst, sum(left, right));
        }
        _ => cpu.exception = (ExceptionCode::UnimplementedOp, dst.size as u64).into(),
    }
}

pub const ARCHITECTURAL_FAULT: u64 = 0x5341440000;

fn reg(cpu: &Cpu, name: &str) -> VarNode {
    cpu.arch.sleigh.get_reg(name).unwrap().get_raw_var()
}

pub fn restore_legacy_controls(cpu: &mut Cpu) {
    // Previous user-mode snapshots left both control registers zero, an invalid long-mode state.
    if cpu.read_var::<u64>(reg(cpu, "CR0")) == 0 && cpu.read_var::<u64>(reg(cpu, "CR4")) == 0 {
        cpu.write_var(reg(cpu, "CR0"), 0x80010033u64);
        cpu.write_var(reg(cpu, "CR4"), 0x40620u64);
    }
}

fn fault(cpu: &mut Cpu, vector: u64) {
    cpu.exception = if vector == 6 {
        (ExceptionCode::InvalidInstruction, cpu.read_pc()).into()
    } else {
        (ExceptionCode::Environment, ARCHITECTURAL_FAULT | vector).into()
    };
}

fn check(cpu: &mut Cpu, _: VarNode, args: [Value; 2]) {
    let family = cpu.read::<u8>(args[0]);
    let cr0 = cpu.read_var::<u64>(reg(cpu, "CR0"));
    let cr4 = cpu.read_var::<u64>(reg(cpu, "CR4"));
    let enabled = if family == 2 {
        cr4 & (1 << 18) != 0 && cpu.read_var::<u64>(reg(cpu, "XCR0")) & 6 == 6
    } else {
        cr0 & 4 == 0 && (family == 0 || cr4 & (1 << 9) != 0)
    };
    if !enabled {
        fault(cpu, 6);
    } else if cr0 & 8 != 0 {
        fault(cpu, 7);
    } else if family == 0 {
        let status = cpu.read_var::<u16>(reg(cpu, "FPUStatusWord"));
        let control = cpu.read_var::<u16>(reg(cpu, "FPUControlWord"));
        if status & !control & 0x3f != 0 {
            fault(cpu, 16);
        }
    }
}

fn stack_operand(cpu: &mut Cpu) -> bool {
    let pc = cpu.read_pc();
    let mut offset = 0;
    let mut segment = None;
    let byte = |cpu: &mut Cpu, offset| {
        cpu.mem
            .read::<1>(pc + offset, icicle_cpu::mem::perm::NONE)
            .unwrap()[0]
    };
    loop {
        match byte(cpu, offset) {
            0x36 => segment = Some(true),
            0x26 | 0x2e | 0x3e | 0x64 | 0x65 => segment = Some(false),
            0x40..=0x4f | 0x66 | 0x67 | 0xf2 | 0xf3 => {}
            _ => break,
        }
        offset += 1;
    }
    if let Some(stack) = segment {
        return stack;
    }
    offset += match byte(cpu, offset) {
        0xc5 => 3,
        0xc4 => 4,
        _ => 2,
    };
    let modrm = byte(cpu, offset);
    let mode = modrm >> 6;
    let base = modrm & 7;
    if base == 4 {
        let sib_base = byte(cpu, offset + 1) & 7;
        sib_base == 4 || (sib_base == 5 && mode != 0)
    } else {
        base == 5 && mode != 0
    }
}

fn memory(cpu: &mut Cpu, dst: VarNode, args: [Value; 2]) {
    let address = cpu.read::<u64>(args[0]);
    let aligned_sse = cpu.read::<u8>(args[1]) != 0;
    let canonical = |value: u64| (((value as i64) << 16) >> 16) as u64 == value;
    if !canonical(address)
        || !address
            .checked_add(dst.size as u64 - 1)
            .is_some_and(canonical)
    {
        let vector = if stack_operand(cpu) { 12 } else { 13 };
        fault(cpu, vector);
        return;
    }
    if aligned_sse && address & 15 != 0 {
        fault(cpu, 13);
        return;
    }
    let mut bytes = [0u8; 16];
    for offset in 0..dst.size as usize {
        match cpu
            .mem
            .read::<1>(address + offset as u64, icicle_cpu::mem::perm::READ)
        {
            Ok(value) => bytes[offset] = value[0],
            Err(error) => {
                cpu.exception = (
                    ExceptionCode::from_load_error(error),
                    address + offset as u64,
                )
                    .into();
                return;
            }
        }
    }
    if dst.size == 8
        && address & 7 != 0
        && cpu.read_var::<u64>(reg(cpu, "CR0")) & (1 << 18) != 0
        && cpu.read_var::<u8>(reg(cpu, "AC")) != 0
        && cpu.read_var::<u16>(reg(cpu, "CS")) & 3 == 3
    {
        fault(cpu, 17);
        return;
    }
    if dst.size == 8 {
        cpu.write_var(dst, u64::from_le_bytes(bytes[..8].try_into().unwrap()));
    } else {
        cpu.write_var(dst, u128::from_le_bytes(bytes));
    }
}

#[cfg(test)]
mod tests {
    use super::sum;

    #[test]
    fn unsigned_extremes_and_independent_groups() {
        assert_eq!(sum([0; 8], [255; 8]), [0xf8, 7, 0, 0, 0, 0, 0, 0]);
        let mut left = [0; 32];
        for lane in 0..4 {
            left[lane * 8..lane * 8 + 8].fill((lane * 71) as u8);
        }
        let result = sum(left, [0; 32]);
        for lane in 0..4 {
            assert_eq!(
                u64::from_le_bytes(result[lane * 8..lane * 8 + 8].try_into().unwrap()),
                lane as u64 * 568
            );
        }
        assert_eq!(sum(left, left), [0; 32]);
    }

    #[test]
    fn all_byte_pairs_match_unsigned_subtraction() {
        for a in 0..=255u8 {
            for b in 0..=255u8 {
                assert_eq!(
                    u64::from_le_bytes(sum([a; 8], [b; 8])),
                    (i16::from(a) - i16::from(b)).unsigned_abs() as u64 * 8
                );
            }
        }
    }

    #[cfg(target_arch = "x86_64")]
    #[test]
    fn random_vectors_match_sse2_hardware() {
        use std::arch::x86_64::{__m128i, _mm_sad_epu8};
        let mut seed = 0x123456789ABCDEF013579BDF2468ACE0u128;
        for _ in 0..4096 {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            let other = seed.rotate_left(49) ^ 0xFF00AA5555AA00FF;
            let expected = unsafe {
                std::mem::transmute::<__m128i, [u8; 16]>(_mm_sad_epu8(
                    std::mem::transmute(seed),
                    std::mem::transmute(other),
                ))
            };
            assert_eq!(sum(seed.to_le_bytes(), other.to_le_bytes()), expected);
        }
    }
}

fn enter_mmx(cpu: &mut Cpu, _: VarNode, _: [Value; 2]) {
    let status = cpu
        .arch
        .sleigh
        .get_reg("FPUStatusWord")
        .unwrap()
        .get_raw_var();
    let top = (cpu.read_var::<u16>(status) >> 11) as usize & 7;
    if top == 0 {
        return;
    }
    // SLEIGH stores ST0..ST7 in logical stack order; MMX addresses physical R0..R7.
    let regs: [_; 8] = std::array::from_fn(|i| {
        cpu.arch
            .sleigh
            .get_reg(&format!("ST{i}"))
            .unwrap()
            .get_raw_var()
    });
    let values: [[u8; 10]; 8] = std::array::from_fn(|i| cpu.read_var(regs[i]));
    for physical in 0..8 {
        cpu.write_var(regs[physical], values[(physical + 8 - top) & 7]);
    }
}
