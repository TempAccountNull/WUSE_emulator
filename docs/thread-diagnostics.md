# Read-only guest thread diagnostics

Selecting a debugger thread activates it in the scheduler. That can evaluate
waits, initialize its context or dispatch an APC. Inspecting a blocked process
through repeated thread selection can therefore change the state under study.

The Windows handler advertises `qXfer:sogen-threads:read+`. Clients read XML
with `qXfer:sogen-threads:read::OFFSET,LENGTH`, using hexadecimal byte offsets.
The query reports stored thread state without selecting threads or evaluating
wait predicates: handles, thread dependencies, instruction counts, module/RVA
locations, suspension, exit status, waits, pending APCs and message counts.
RIP is reported only for the active thread. Inactive threads report their last
recorded instruction address. Retained exited threads remain visible.

## Validation

Release and tidy builds passed. Both new tests pass in interpreter and JIT
modes. The tidy run also passes all ten FileSectionViewTest and
SystemHandleInformationTest cases in each mode. Tests verify that inspection
preserves registers, active selection, pending APCs, waits and initialization.
The CLI guest smoke completed 29 of 30 cases in 27.67 seconds; the existing
APIs case failed. This is not a clean whole-suite result.

An actual Destiny checkpoint query returned 25 threads. All instruction counts
and last-instruction addresses match the live continuation's initial inventory
from the same checkpoint. The selected thread and complete GDB register packet
were unchanged before and after inspection. No continue or step was issued in
the isolated inspector.

Thread 0x8 waits on handle 0x4800015, which resolves to thread 0x8C. That thread
starts at destiny2.exe+0x111C960 and waits on semaphore 0x3800018. The verified
dump identifies its entry as the Graphics Heartbeat worker. Thread 0x94 starts
at destiny2.exe+0x16E1580, the game's MessageBoxW error routine. Its saved stack
contains: "Graphics runtime detected a crash or loss of device."

This identifies the startup wait chain; it does not fix the preceding graphics
failure or establish Sunrise boot. Local evidence is in
`sogen-snapshot-threads-1789410000476314200`, including wait-chain-proof.json and
raw saved stacks. The input checkpoint SHA-256 is
`c0f69fd56a282caf35af51715220cc44544e049cda85438d962e9d11fe3bc2f7`.
