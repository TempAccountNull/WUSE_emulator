# Bounded audit logging and host output resilience

A 3.5 hour Destiny 2 replay wrote 176 GB of `events.jsonl` and 84 GB of console text
(about 20 MB/s) until D: reached zero bytes free. The analyzer then died with
`0xC0000409` / `FAST_FAIL_FATAL_APP_EXIT`: the console `async_file_writer` had stored the
failed write, `logger::error` rethrew it while the run-failure packet was being reported,
and `logger::warn` rethrew it again from `~windows_emulator` during unwinding, which is
`terminate()` (see `benchmarks/sogen-fixes/crashes/analyzer-64244-20260916-233401`).

## Logger

`logger::print_message` never throws. When the console writer fails (exhausted volume,
closed handle) the writer is retired once, a red `[logger] console output writer failed`
notice goes to stderr, and later lines are written synchronously to stderr with their
colors. `logger::set_console_output` lets tests inject a failing writer.

## Analyzer exit codes

| code | meaning |
| --- | --- |
| 0 | guest exited with `STATUS_SUCCESS`, or a checkpoint was saved |
| 1 | guest exited with another status |
| 2 | host-side emulation or reporting failure (`run_failed` event emitted or, when the reporters fail too, printed to stderr) |

`emit_failure` reports through a guarded path: a reporter that throws (for example the
JSONL file on a full disk) no longer escapes the catch handler.

## Graceful stop

The console control handler treats `CTRL_BREAK_EVENT` like `CTRL_C_EVENT`, so a controller
that created the analyzer in its own process group can request the same graceful stop
(first signal stops the run, third signal forces `_Exit`).

## Icicle hooks

Host C++ exceptions raised inside hooks are deferred instead of unwinding through icicle's
Rust `extern "C"` frames (which abort the process). `icicle_x86_64_emulator` implements
`detail::hook_exception_sink`: the first exception is stored, `icicle_stop` is requested,
and `start()` rethrows it after `icicle_start` returns. All hook wrappers (`bind_cpu`,
`execution_hook`, write observation lambdas, deferred actions) route through it.

## Console repeat coalescing (`--console-coalesce-repeats`)

Console only. Consecutive identical function, syscall, object/environment access, foreign
transition, generic access and IOCTL lines of one guest thread are folded:

- the first occurrence prints normally;
- every 1000 repeats (or after one second) a compact summary prints:
  `~ tid 76 repeated 1000 more times [calls 461498001..461499000]: Executing function: memcpy ...`;
- a key change or `flush()` prints the remaining count.

Every event still reaches the structured reporters unchanged. The 29 million identical
`memcpy` console lines of the failed run become a few hundred summaries.

## Report modes (`--report-mode full|audit`)

`full` writes every event (the previous behavior). `audit` keeps diagnostics individually
and counts routine events:

| event | audit behavior |
| --- | --- |
| library-to-library `function_execution` | counted |
| interesting `function_execution` (main-module caller) | first 3 per `fn (module) via caller` retained, then counted |
| `object_access` / `environment_access` | main-module accesses first 3 per key retained, others counted |
| regular `syscall` | counted; inline and crafted syscalls always retained |
| `foreign_code_transition` | interesting first 3 per key, others counted |
| `thread_switch` | counted |
| `generic_access`, `io_control` | first 3 per key retained, then counted |
| everything else (suspicious, prints, faults, threads, modules, progress, memory, ...) | retained |

Counts are written as `event_aggregate` records every 100,000 summarized events or every
5 seconds, with cumulative `byType` totals and the 24 busiest keys of the window; a final
aggregate is written on `flush()`. Memory stays bounded (4096 distinct keys per window,
65536 retained keys).

When `--report` is given, `report-status.json` is rewritten atomically next to the report
about once per second with `mode`, `retained_events`, `summarized_events`,
`summarized_by_type`, `last_event_type` and `last_location` (`rip`, `module`, `tid`, `ic`).
Panels and MCP readers use this file instead of reading the report.

## Validation

`windows-emulator-test.exe --gtest_filter=ConsoleCoalescing.*:AuditReport.*:LoggerResilience.*:IcicleHookExceptions.*`

## Duplicate suppression (`--report-dedupe`)

Identical observations are only worth recording once. With `--report-dedupe` the analyzer keeps a
report record, and prints a console line, only when its *data* differs from one already kept.
Data means every field except the values that change on every occurrence (`ic`, `callCount`,
`call_id`, the stack pointer and the guest pointers of printed-call arguments); the console
compares the same text it would print. Suppressed duplicates are counted:
`report-status.json` carries `dedupe`, `deduplicated_events` and `dedupe_keys`, and the console
prints `~ suppressed N duplicate lines` at most once per summary interval and at flush.

Failure packets (memory violations, fast fails, the run footer), guest stdout and progress
heartbeats are never deduplicated. The first 2^20 distinct records are tracked; beyond that, new
distinct records are still written but no longer remembered.

## Hidden modules (`--hide-module NAME`)

Some modules are noise for a given investigation (a shim DLL such as `steam_api64.dll` that is not
the code under study). `--hide-module NAME` (repeatable, case-insensitive file name) drops every
observation that executes in the module or was reached from it, at both the console and the report;
`report-status.json` counts them as `hidden_events`. Failure packets and run start/end are always
kept. The panel's Modules popover hides the same names on display without the flag; its Launch tab
can ask the runner to pass them as `--hide-module` for the next launch.

## Hidden event types (`--hide-event TYPE`)

`--hide-event TYPE` (repeatable) drops every event of one report type, named as it appears in the
`type` field of the JSONL report (`function_execution`, `object_access`, `syscall`, ...), at both the
console and the report; they count as `hidden_events`. Failure packets and run start/end are
always kept. The panel's Launch tab writes the list for the next launch.
