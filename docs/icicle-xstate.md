# Icicle XSAVE state

The actual game stopped at ntdll.dll+0xA1BC9 on `0F AE 2B`
(`xrstor [rbx]`). Icicle had no XRSTOR helper and left XCR0 zero. The
analyzer disabled AVX in CPUID, while KUSER_SHARED_DATA advertised AVX/MPX
state through EnabledFeatures=0x1F and EnabledVolatileFeatures=0xF.

Icicle now implements standard XSAVE/XSAVE64 and XRSTOR/XRSTOR64 for the
advertised x87/SSE state. XCR0, CPUID leaf 0xD and the guest shared-data
configuration agree on mask 3 and a 576-byte save area. Zero XCR0 in older
register snapshots is migrated to mask 3; shared-data capabilities are
reapplied after restoring process snapshots. Other backends are unchanged.

Requested state is selected by EDX:EAX AND XCR0. XRSTOR restores present
components and initializes absent requested components; an SSE request
loads MXCSR even when XMM state is initialized. The x87 abridged tag word
is expanded using TOP and the saved register classifications. XSAVE leaves
unselected components and reserved bytes untouched. It uses conservative
in-use tracking, with no init optimization or compacted-save support.

Malformed headers, MXCSR reserved bits and misalignment raise #GP. The
Windows dispatcher delivers the general-protection fallback as an access
violation with read/-1 parameters. Memory faults retain their actual address.

## References

- Intel SDM Volume 1, sections 13.6-13.8: state tracking, XSAVE and XRSTOR.
  https://cdrdv2-public.intel.com/874240/325462-090-sdm-vol-1-2abcd-3abcd-4.pdf
- Intel SDM Volume 2D, XSAVE/XRSTOR instruction entries.
  https://cdrdv2-public.intel.com/789589/334569-sdm-vol-2d.pdf
- Verified guest-root ntoskrnl.exe in IDA: KiGeneralProtectionFault RVA
  0x40DBC0 dispatches internal code 0x10000001; KiPreprocessFault RVA
  0x2618D0 maps the unhandled decode case to C0000005, parameters 0/-1.

## Actual-game replay

Input snapshot SHA-256:
`169c8208ba390df93d8c09f38d733b86ed3734c32265b52625c2a35a978201b5`.

The old build stopped without advancing. The new build advanced RIP from
0x1800A1BC9 to 0x1800A1BCC. RSP remained 0xBA5F7350D0. Input at
0xBA5F735480 had XSTATE_BV=0, XCOMP_BV=0 and MXCSR=0x1F80. The saved
request mask was 0xC; its effective mask is zero on this x87/SSE CPU.
This replay does not claim that AVX or MPX registers were restored.
Nonzero x87/SSE restoration is covered separately by the instruction tests.

Local evidence: captures `xrstor-old` and `xrstor-supported`, each containing
`xrstor-proof.json`, register snapshots and logs. The supported output
snapshot SHA-256 is
`b84b4b4cad7b7250313a45e67452be5ba3427c678c1e0e155ef41f4d04069b20`.

The continuation passed the instruction and executed more than 570,000
further traced calls. It also reported a graphics-device failure and entered
KiUserExceptionDispatcher from gdi32full.dll+0x1BFF8; this is a separate
failure under investigation. Sunrise boot is unconfirmed.

## Validation

Release and tidy: 62 selected tests passed in each of interpreter and JIT modes.
This includes both instruction encodings, selective restoration and saves,
fault contexts, shared-data snapshot migration and prior IOCTL/graphics cases.
The CLI smoke test still reaches the existing Message Queue (Paint) failure
with C000041D.
