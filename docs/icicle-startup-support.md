# Icicle startup support

## Execution fixes

| Failure | Change | Check |
| --- | --- | --- |
| Self-modifying code repeats a faulting store or uses stale code | Clear executed-page guards and translation state; resume the failed pcode store before advancing | Host patches, same-block guest patches, cross-page stores, stack-store continuation |
| RDTSC/RDTSCP bypass Windows timestamp hooks | Dispatch decoded timestamp instructions with their full length and continuation result | Prefixes, outputs, finalized RIP, opcode bytes inside immediates |
| Legacy DIVPS/DIVPD raises UnimplementedOp | Lower register and memory forms to per-lane floating-point division | Four-lane memory input, source preservation, special values, aliased DIVPD |
| RSQRTPS/RSQRTSS raises UnimplementedOp | Register packed/scalar helpers; preserve unaffected destination lanes | Register/memory forms, denormals, signed zero, infinities, NaN payloads, MXCSR invariance |
| YMM reads truncate; writes panic | Transfer both 128-bit halves | Full-width round trip and upper-lane preservation |

On x86-64, reciprocal square root uses the host SSE estimate. Other hosts use a portable implementation within the architectural error bound. General floating-point exception and rounding fidelity is unchanged.

## Execution cost

`SOGEN_ICICLE_JIT=1` enables the existing JIT. Instruction callbacks remain enabled.

The single-vCPU cooperative path holds the kernel mutex for an execution slice; callbacks on that thread borrow it. Other host threads remain excluded. Multi-vCPU locking is unchanged. Empty callback lists and repeated module/section lookups avoid unnecessary work. The callback guard restores nested hook state directly.

Icicle remains single-vCPU. These changes do not parallelize guest instructions.

## Capture and resume

```powershell
$env:SOGEN_ICICLE_JIT = '1'
$env:FORCE_COLOR = '1'
.\analyzer.exe --backend icicle -e C:\guest-root -v --snapshot-out startup.snap guest.exe
.\analyzer.exe --backend icicle -e C:\guest-root -v -a startup.snap --snapshot-out next.snap guest.exe
```

`--snapshot-out` saves on stop, failure or debugger disconnect. It prevents an automatic fresh restart after disconnect. EOF at the interactive prompt returns without spinning. Snapshot restore does not roll back files exposed through host mappings.

Verbose output reports instruction rate and module RVA every five seconds. `FORCE_COLOR=1` preserves ANSI spans in redirected Windows output. JSONL event ordering is preserved; bounded background writers flush batches and drain on explicit flush.

## Validation

- VS2022 RelWithDebInfo build and tidy build pass.
- 25 focused tests pass with both interpreter and JIT; two Rust reciprocal-square-root tests pass.
- Actual guest execution passed the previous DIVPS and RSQRTPS stops. Captured operands, results, unchanged registers and return addresses were checked.
- Actual guest snapshot restore reproduced the sampled startup state and next instruction.
- CLI smoke still fails at the existing Window Geometry case with C000041D. Earlier thread, exception, TLS, socket and APC checks pass.
- Full application boot remains unconfirmed.

Earlier GDB captures from the old bridge cannot establish upper YMM values because its read path returned zeros for that half. XMM captures and raw serialized register storage are separate paths.
