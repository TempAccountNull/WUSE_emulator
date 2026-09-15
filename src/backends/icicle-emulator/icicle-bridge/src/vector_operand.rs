use icicle_cpu::{Cpu, Exception, ExceptionCode, ValueSource};
use pcode::VarNode;

pub fn reg(cpu: &Cpu, name: &str) -> VarNode {
    cpu.arch.sleigh.get_reg(name).unwrap().get_raw_var()
}

pub fn fault(cpu: &Cpu, vector: u64) -> Exception {
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

pub struct MemoryOperand {
    pub modrm: u8,
    pub b: u8,
    pub x: u8,
    pub address32: bool,
    pub segment: u8,
    pub scale: usize,
}

impl MemoryOperand {
    pub fn address(
        &self,
        cpu: &Cpu,
        bytes: &[u8],
        mut offset: usize,
        next: u64,
    ) -> Result<(u64, bool), Exception> {
        let Self {
            modrm,
            b,
            x,
            address32,
            segment,
            scale,
        } = *self;
        let invalid = || fault(cpu, 6);
        let mode = modrm >> 6;
        let rm = modrm & 7;
        let mut stack = false;
        let gp = |index: usize| {
            cpu.read_var::<u64>(reg(
                cpu,
                [
                    "RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI", "R8", "R9", "R10",
                    "R11", "R12", "R13", "R14", "R15",
                ][index],
            ))
        };
        let mut address = 0u64;
        let mut displacement32 = mode == 2;
        if rm == 4 {
            let sib = *bytes.get(offset).ok_or_else(invalid)?;
            offset += 1;
            let base = sib & 7;
            let index = (sib >> 3) & 7;
            if mode == 0 && base == 5 {
                displacement32 = true;
            } else {
                address = gp((base + 8 * b) as usize);
                stack = base == 4 || base == 5;
            }
            if index != 4 || x != 0 {
                address = address.wrapping_add(gp((index + 8 * x) as usize) << (sib >> 6));
            }
        } else if mode == 0 && rm == 5 {
            address = next;
            displacement32 = true;
        } else {
            address = gp((rm + 8 * b) as usize);
            stack = rm == 5;
        }
        if mode == 1 {
            let displacement = *bytes.get(offset).ok_or_else(invalid)? as i8 as i64;
            offset += 1;
            address = address.wrapping_add((displacement * scale as i64) as u64);
        } else if displacement32 {
            let value = bytes.get(offset..offset + 4).ok_or_else(invalid)?;
            offset += 4;
            address =
                address.wrapping_add(i32::from_le_bytes(value.try_into().unwrap()) as i64 as u64);
        }
        if offset != bytes.len() {
            return Err(invalid());
        }
        if address32 {
            address = address as u32 as u64;
        }
        if segment != 0 {
            stack = segment == 0x36;
        }
        if segment == 0x64 || segment == 0x65 {
            address = address.wrapping_add(cpu.read_var::<u64>(reg(
                cpu,
                if segment == 0x64 {
                    "FS_OFFSET"
                } else {
                    "GS_OFFSET"
                },
            )));
        }
        Ok((address, stack))
    }
}
