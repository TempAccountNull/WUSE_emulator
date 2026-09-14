# Process ASLR policy

NtSetInformationProcess class 0x34 now handles ProcessASLRPolicy. Queries return
the effective flags. Enabled bottom-up randomization, forced relocation and
stripped-image rejection cannot be cleared. High-entropy eligibility is fixed
at process creation. Reserved bits, invalid combinations, input lengths,
alignment, inaccessible buffers and invalid handles return errors without
changing the policy.

Automatic allocations use a randomized origin. Forced image mappings choose a
randomized base and apply the PE relocations. NtMapViewOfSection reports
STATUS_IMAGE_NOT_AT_BASE for a relocated image and STATUS_ILLEGAL_DLL_RELOCATION
for a rejected RELOCS_STRIPPED image. Existing mapped pages are preserved.
WOW64 thread setup preserves a randomized allocation origin below 4 GB.

The policy and PRNG state are saved in a tagged snapshot extension. Legacy
snapshots initialize policy for future allocations from the executable flags.
Relative-time runs use a repeatable seed; other runs use std::random_device.
Snapshots saved before process setup remain loadable. Initial placement of the
executable, ntdll and win32u still follows Sogen's existing mapping order. The
address distribution is an emulator implementation, not a reproduction of
every Windows ASLR placement rule.

## References and native checks

The guest-root ntoskrnl.exe in IDA confirms the setter's validation order,
monotonic bits and immutable high-entropy flag. Disposable native Windows
processes confirm:

- Initial flags 0x5; requesting 0xF then 0xB succeeds and queries as 0xF.
- Clearing an enabled bit returns 0xC0000022.
- Requesting 0x9 returns 0xC0000030; requesting 0x11 returns 0xC000000D.
- Mapping a RELOCS_STRIPPED image under 0xF returns 0xC0000269.
- A relocatable image without DYNAMIC_BASE maps with status 0x40000003.
- Clearing its relocation directory without setting RELOCS_STRIPPED still maps.

The image probes use a copy of test-sample.exe and never execute mapped code.

https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-process_mitigation_aslr_policy

https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessmitigationpolicy

## Actual Destiny return

The game requested policy 1 with flags 0xF, using an eight-byte input at
0x1019CF6B8. The original checkpoint was taken after the unsupported syscall.
The comparison restores RCX from R10 and moves RIP to the verified ntdll stub
entry. That stub restores its own syscall number and condition flags. Guest
memory and syscall results are not patched.

```text
ntdll.dll+0x9D9C0: syscall stub entry
ntdll.dll+0x9D9D4: RET
Old RAX:          0xC00000BB
Corrected RAX:    0x00000000

Before RET RSP:   0x1019CF668 / thread 8 stack+0x2FF668
Saved bytes:      69 E1 45 04 01 00 00 00
After RET RIP:    0x10445E169 / kernelbase.dll+0x7E169
After RET RSP:    0x1019CF670

Wrapper RET RIP:  0x10445E20B / kernelbase.dll+0x7E20B
Wrapper RET RSP:  0x1019CF698
Saved bytes:      86 7B 3A 40 01 00 00 00
Destination:      0x1403A7B86 / destiny2.exe+0x3A7B86
Result:           TRUE / RAX=1
```

Seven wrapper instructions were stepped. TEST EAX,EAX sets SF=0 and the JNS
branch is taken to kernelbase.dll+0x7E201. Both RET destinations match their
saved stack slots. The game continues from the corrected checkpoint. Sunrise
boot is not yet confirmed.

Evidence is in PatchScanner/benchmarks/sogen-fixes/aslr-validation.json,
aslr-kernel-reference.json, native-aslr-policy.json, native-aslr-images and
destiny-sogen-1789390447576844400-aslr-corrected/aslr-proof.json.

## Validation

Release and tidy builds pass. Each build passes 124 startup tests in each of
Icicle's interpreter and JIT modes. Twelve ASLR tests cover validation, queries,
allocation origins, actual image-map statuses and relocated pointers,
pre-setup/legacy snapshots and repeatable layouts.

The CLI smoke passes Window Geometry, User Callback, Mutable User Callback and
Message Queue (General), then ends with C000041D during Message Queue (Paint).
This separate failure is retained; it is not a passing full smoke run.

Completed SHA and decryption milestones survive checkpoint continuations in
the external PowerShell viewer. The inheritance check preserves all 38 SHA
checks, four AES blocks and three completed execution milestones.

The actual game reached the configured 300,000-call checkpoint in LocalAlloc
after 596.82 seconds. Its saved state then continued without a call-count limit
into steam_api64.dll and Windows cryptography routines. These checkpoints prove
progress beyond the original failure; they do not establish a completed boot.
