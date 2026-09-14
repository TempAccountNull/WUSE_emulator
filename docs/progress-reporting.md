# Progress after snapshot restore

Initialize the progress clock and instruction baseline when analysis callbacks
are registered. Snapshot instruction totals remain in the report; the first
rate uses only instructions counted after that baseline.

The Destiny continuation's original first sample reported 2,010,000,449 inst/s
at 5,007 ms because it divided the restored total by the new run's elapsed
time. Later interval samples were correct.

The regression executes two guest time slices, serializes and restores the
emulator, then requests a progress sample without executing more instructions.
The original implementation reports 43,690 inst/s; the corrected one reports
zero while retaining the cumulative instruction total.

Release: 98 tests pass in interpreter and JIT modes.
Tidy: build passes; all three observation tests pass in both modes.
The CLI smoke retains its separate Window Geometry C000041D exception.

Evidence: PatchScanner/benchmarks/sogen-fixes/progress-restoration-validation.json.

## External colored log viewer

PatchScanner's viewer keeps one progress panel beneath verbose output. It
refreshes once per second and redraws after each bounded 64 KiB read. Completed
hash and decryption milestones remain visible when a later syscall stops.
Current phase percentage, instruction rate, elapsed time and module-relative
instruction address are separate fields.

A saved-log replay with 11,000 lines verifies console-buffer scrolling, one
remaining panel, fractional percentages, split-line text, ANSI colors and
highlights, terminal state and an unchanged idle cursor position. It does not
execute the guest. The reopened viewer also displays the actual Destiny capture:
SHA-384 100%, decryption marker reached, four verified AES blocks, and the
NtSetInformationProcess class 0x34 stop.

Evidence: PatchScanner/benchmarks/sogen-fixes/progress-panel-validation.json
and progress-panel-live.json. Viewer source remains in PatchScanner/scripts.
