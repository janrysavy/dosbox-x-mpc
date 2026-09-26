# CMOS save-state coverage

`src/hardware/cmos.cpp` previously saved register bytes and timer fields but
omitted the separate live calendar, alarm, decoded BCD/12-hour/lock flags,
`clock_time_t`, and the synchronization values consumed by the BIOS. Restoring
register B alone does not re-run its write handler to reconstruct those flags.

The `CMOS-2` component adds each omitted scalar explicitly. Its new name makes
the component inventory incompatible with the old `CMOS` layout. Old native
save-state files and cross-build checkpoints are not supported by this change.
It does not add persistent agent import/export or make the legacy file loader
transactional. In-memory checkpoints created by one running old binary remain
usable by that same binary.

## Native regression evidence

`tests/agent/cmos_state_tests.inc` is included in the production translation unit
for agent debug builds. It calls the actual serializer, port 70h/71h handlers,
and timer callback. The fixture saves/restores the complete machine around each
test so scheduled events and interrupt-controller state do not leak into later
tests. It also restores omitted fields directly during teardown so the
old-serializer negative control is isolated.

The four cases cover:

- Calendar, alarm, decoded mode/lock flags, host-sync pending values, and a saved
  index selection before its data-port read.
- A locked clock through `cmos_timerevent`, which is where lock is checked.
- Midnight from 2000-02-27 23:59:59 to February 28, compared with uninterrupted
  execution, including alarm/update flags and status-C read-to-clear behaviour.
- BCD and 12-hour write interpretation after a mode change and restore.

The timer cases invoke the production callback at a due one-second boundary,
without guest instruction execution. They prove component continuation, not
cold-restart parity for the complete emulator or instruction-to-IRQ latency.
Tests set `sync_time=false`. With host synchronization enabled, `cmos_tick`
consults `time(NULL)` and can diverge after a restart delay; recording CMOS fields
does not virtualize host time. `sync_time` itself is configuration, not changed
by this component.

One separate open snapshot debt is `CPU_NMI_gate`: CMOS port-index writes update
it in `src/hardware/cmos.cpp:254`, and CPU interrupt delivery consumes it in
`src/cpu/cpu.cpp:610` (the global is defined at line 75). The CPU serializer at
`src/cpu/cpu.cpp:4934` does not save that global, and loading CMOS fields does not
replay the port write. This omission was verified in source, not by a live NMI
continuation probe; this slice's tests restore the host test fixture's gate
explicitly and do not establish full-machine NMI parity.

Build: Windows x64, VS 18 Insiders, `Agent Debug SDL2`; isolated fork worktree.
Run: `GTEST_FILTER=CmosState.* python tests/agent/run_gtests.py` (set the environment
variable using the host shell). Full native suite uses the same command without
the filter, after `tests/agent/build_fixture.ps1`.

The original pinned-lineage build passed **86 native tests**, including all four CMOS cases.
Replacing only `cmos.cpp` with the baseline `7832299` version and appending the
same four regression cases produced **four failures out of four**. The fixed source was
then restored, rebuilt and the full suite passed again. The complete final suite,
negative-control output, and initial calendar failure are retained in
[`cmos-state-20260922.txt`](../tests/agent/evidence/cmos-state-20260922.txt), including
the final executable SHA-256. No cold-restart claim follows from these runs.
The negative run preceded a test-fixture guard for failed initial capture;
that guard changes teardown only on capture failure, not the exercised assertions.

For the pull request, the CMOS-only commit was applied to remote base `03bca583`.
The isolated build there passes **85 native tests**; the difference is the local
memory-change watcher test, which is not part of this PR. Its complete output and
binary SHA-256 are in
[`cmos-pr-20260922.txt`](../tests/agent/evidence/cmos-pr-20260922.txt).
`agent-native.yml` adds a focused Windows agent-debug build/test job because the
existing platform workflows exercise other build configurations, not these
agent-only CMOS regressions. CI success is separate evidence from the local run.
The first hosted job built successfully but lacked `llvm-mc.exe` when generating
fixtures (run `35727489016`). The fixture builder now offers `-UseClang`, using
Clang's integrated assembler and `lld-link -flavor gnu`; the workflow selects it.
Locally both tool paths produce the identical `AGFILE.COM` SHA-256
`9fbf74c755003fd92afd0bf80739f3927f4c1c27c0895acb92d0986980c1073d`, and the full
85-test suite passes after rebuilding fixtures with that option.

## Separate calendar defect discovered by the initial test

The first midnight fixture programmed **2000-02-28 23:59:59**, binary/24-hour,
host synchronization disabled. One real timer callback produced **2000-03-01
00:00:00**, while the expected calendar result was **2000-02-29 00:00:00**.
Both uninterrupted and restored branches produced the same wrong date. The
initial test log is retained with this slice's evidence.

Source conditions in `cmos_tick` explain the observed early rollover:

```cpp
if (++cmos.clock.day < mdays) return;
// ... day = 1 ...
if (++cmos.clock.month < 12) return;
```

The day comparison advances the month on its last day; the month comparison
also predicts skipping December when advancing from November. Only the
February boundary above was measured in the persistence slice. Its test uses
February 27 to 28, while preserving the failed boundary measurement here.

## Calendar boundary follow-up

The separate calendar fix changes both pre-increment comparisons to `<=`.
Valid days include the last day of a month; valid months include December.
`CalendarKeepsLastDayAndDecemberBeforeRollingOver` uses the registered CMOS
port handlers and directly invokes `cmos_timerevent` at a one-second boundary
for 12 explicit calendar transitions: 1900/2000 century cases, an ordinary leap
year, 30/31-day months, entry into December, and year rollover. It also verifies
weekday rollover and zero hours/minutes/seconds at midnight.

Before the fix, the native test failed for seven of those dates, including
November 30, 2023 advancing straight to January 1, 2024. With the fix, the
original pinned-lineage build passes **87 native tests**, including all 12 date
cases and the existing persistence cases. Full logs and executable SHA-256 are in
[`cmos-calendar-20260922.txt`](../tests/agent/evidence/cmos-calendar-20260922.txt).
This verifies the listed callback boundaries with host synchronization disabled;
it does not exercise PIC event dispatch, guest `IN/OUT` instructions, complete
machine restart, or guest instruction timing parity.

An independent review requested stronger negative-run provenance. The evidence
now records the old source and unchanged test commit identities, SHA-256 values
for both source files and the negative executable, the exact isolated build
launcher, its successful build result, the failing native run, and the restored
positive run (87 tests). Reconstruct the negative case with `src/hardware/cmos.cpp`
from `50fe270` and `tests/agent/cmos_state_tests.inc` from `af7b85a`; the former
contains the two original strict comparisons, the latter the 12-case test.

## Broader restartability audit

The source-only [remaining state audit](PERSISTENT_STATE_DEBT.md) records the
entire omitted CPU NMI triplet and open-file pending timestamp/I/O state, with
exact source locations and proposed native/cold-process probes. These remain
open debt; the CMOS tests and matching disk bytes do not prove them covered.

The calendar follow-up was then prepared separately on CMOS PR head `256c2cb`.
Its production CMOS source and tests match reviewed `f517817` byte-for-byte.
The fresh isolated build passes **86 native tests** (one local watcher test is
absent from the remote lineage); the appended evidence records its executable
hash and full output. This preparation is local; no hosted CI or merge is claimed.

On 2026-09-26 the calendar branch was combined with the pinned debugger
integration base `f763b7b61`. That base's `cmos.cpp` blob is exactly
`c3b68be49a99130ed101955ad8433fa6e5fb52a9`, the old source used in the
recorded failing control. The combined tree's calendar test blob is exactly
`8ef5aca2eb7808940a513bec7e38e67c99eebeb9`, the test used there.
An Insiders MSVC `Agent Debug SDL2` build succeeded with zero errors and all
**87 native tests passed**. The
[compressed full build log](../tests/agent/evidence/cmos-calendar-build-20260926.log.gz)
and [test transcript](../tests/agent/evidence/cmos-calendar-integration-20260926.txt)
retain the executable and source hashes and complete test output.
This trace records the local combined-tree result; hosted CI is tracked on
[PR #4](https://github.com/janrysavy/dosbox-x-mpc/pull/4).
