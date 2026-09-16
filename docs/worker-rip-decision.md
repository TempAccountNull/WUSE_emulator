# WARP worker rejected by the game

The actual-game replay now establishes the path from a sampled worker RIP to
NtTerminateThread. The earlier correlation in `warp-termination-followup.md`
is replaced by executed branch evidence for this invocation.

## Executed path

The game opened worker TID 12 with desired access 0xB, suspended it, and obtained
its context. Sogen returned handle 0x4800002 and saved RIP 0x148CF65A8A, inside
generated WARP code outside the loaded images. These calls returned at game
RVAs 0x26DFCB, 0x290194 and 0x267514.

| Game RVA | Observation |
| --- | --- |
| 0x3AF7BF | Calls 0x3A8BE0 with the sampled RIP in RDX. |
| 0x3A8C55 | A subordinate check rejects the interval 0x12B3E90000..0x12B3FA9400 for this RIP. |
| 0x8326B4B / 0x852F861 | Checks module base and end after a separate four-entry range table. |
| 0x3AF7C4 | Classifier returns RAX=0. |
| 0x579990E | TEST RAX,RAX sets ZF. |
| 0x6D1B2C3 | CMOVNE does not select the nonzero-result destination 0x3AF88E. |
| 0x3AF983 | The executed zero-result path calls 0x290510 with handle 0x4800002 and exit status 0. |
| 0x2933E0 | The wrapper searches ntdll exports for its constructed string NtTerminateThread. |
| 0x293793 | Return address at the subsequent copied syscall entry. |

At syscall address 0x12B3F2D0B2, code begins 0F 05 C3, EAX=0x53,
RCX=R10=0x4800002, and RDX=0. The debugger saved before executing it.
No game instruction or register was patched. The observed classifier uses
address values and module ranges; it is not established as a MEM_PRIVATE or
page-protection test. Generated code must not be relabeled as an image to
change this result.

## Evidence and coverage

Under PatchScanner/benchmarks:

- `destiny-sogen-1789515603914673200-after-memory-walk-fix`: syscall argument
  and return journal; held worker context.
- `destiny-sogen-1789516149258563100-worker-context-consumer-jit`: 12,000
  executed instruction records starting after the context RIP read.
- `destiny-sogen-1789516314960095100-worker-decision-continued`: another
  150,000 instruction records, ending during export-name search.
- `destiny-sogen-1789516653452653200-worker-termination-after-classifier`:
  controlled resume to the verified syscall, with arguments and thread state.

The final resolver interval is covered by the resumed endpoint, not a complete
instruction trace. Operand previews for LEA and NOP are not guest memory
accesses. The requested-name proof uses actual MOV/XOR stores and MOVZX reads.
The endpoint checkpoint is 333,789,086 bytes, SHA256
`7d7454550a613d5fb1671b1b631ad02765d98f105b599476209c4e342f408bd8`.

The copied gate was verified against guest ntdll CodeView identity and bytes.
Full selected records and interpretation are under
`artifacts/source-crosschecks/worker-rip-termination-predicate.json` and `.md`.
Completed bulk logs were pruned only after preserving checkpoints, journals,
focused records, hashes and first/last samples. No boot success is established.

## Separate emulator defects

The fixed zero-address memory query now succeeds in the game. Its following
85 queries form continuous spans through 0x100000000; all return success and
48 bytes. Worker inspection and termination remain reachable after that fix.

The matched kernel requires THREAD_QUERY_INFORMATION (0x40) for class 9.
At kernel RVA 0x8451A3 it forms this access mask and calls the object manager
at 0x8451A9. Missing granted rights return STATUS_ACCESS_DENIED. Sogen ignores
NtOpenThread's requested access and aliases handles; the complete correction
requires the handle model described in `thread-handle-contract.md`.
In this replay, class 9 used handle 0x4800001, while the rejected worker was
0x4800002. The rights defect is not established as the cause of this path.

The same memory walk receives incorrect KUSER allocation metadata. Matched
MiAllocateProcessVads/MiAllocateVad establish readonly/private metadata;
Sogen reports original protection PAGE_NOACCESS and MEM_MAPPED. Its MMIO
backend also silently accepts writes. Fixing only query fields would leave
inconsistent behavior; permissions and snapshot restoration need joint tests.
Exact contracts and raw kernel evidence are retained in
`thread-start-address-class9-contract.md` and
`memory-walk-after-fix-and-kuser-audit.md` under source-crosschecks.

## Graphics path to inspect next

Sogen's NtGdiDdDDIQueryAdapterInfo currently returns d3d10warp.dll for
KMTQAITYPE_UMDRIVERNAME. That supplies a concrete emulator source for WARP
selection, independent of Dawn's optional temporary graphics probe. The exact
game/DXGI selection calls still need to be captured. Cross-check the adapter
contract and matched graphics-kernel implementation before changing it.

The trace established the game's decision, not a missing syscall to suppress.
The pending work is correct graphics and OS behavior, with fresh actual-game
validation. The saved checkpoint remains before worker termination.
