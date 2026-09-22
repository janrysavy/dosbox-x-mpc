# CMOS fork integration: final CI and merge evidence

Observed 2026-09-22. These are component persistence and build results, not proof
of complete restartable emulator snapshots. Remaining debt is in
`docs/PERSISTENT_STATE_DEBT.md` and `docs/CMOS_STATE.md`.

## Final heads and hosted checks

| PR | Tested head | Retained PR workflows | Child jobs | Merge |
|---|---|---|---|---|
| [3](https://github.com/janrysavy/dosbox-x-mpc/pull/3) | `dfb8cdb2e354d3c977a9c75a6e35ab88e7817ca2` | 11/11 success | 28/28 success | `44424e3ebe8ebc0c71bc1e060710979ebfb814b0` |
| [1](https://github.com/janrysavy/dosbox-x-mpc/pull/1) | `b7414809f4b810d6a4771a9f68f9baa2d4279ec8` | 12/12 success | 29/29 success | `a5f3c4e489a5cae7c10464adb6d85f218da589ac` |

The audit selected `pull_request` runs at the exact head SHA and checked every
child job through the Actions jobs API. Zero failed or pending children remained
at each merge. Cancelled duplicate push runs were not used as passing evidence;
every inherited PR workflow was retained. PR2 was included in PR3 and GitHub
automatically marked it merged; `341e082f971826073ef545b0e93b9c233ab93c0b` is an
ancestor of the PR3 merge.

Selected final-head logs:

- [PR3 debugger matrix and explicit MSVC fallback probe](https://github.com/janrysavy/dosbox-x-mpc/actions/runs/35743355056)
- [PR3 full MinGW builds including Windows 9x](https://github.com/janrysavy/dosbox-x-mpc/actions/runs/35743355012)
- [PR3 final MinGW64 workflow](https://github.com/janrysavy/dosbox-x-mpc/actions/runs/35743354977)
- [CMOS native regression: 85 tests passed](https://github.com/janrysavy/dosbox-x-mpc/actions/runs/35743357729)
- [CMOS full MinGW builds including Windows 9x](https://github.com/janrysavy/dosbox-x-mpc/actions/runs/35743357593)
- [CMOS final installer packaging](https://github.com/janrysavy/dosbox-x-mpc/actions/runs/35743357577)

Independent read-only reviews were resolved before merge. The final fallback/CI
review reported no findings; actual MSVC positive and old-source negative-control
results are retained in `mcp-disabled-link-20260922.txt`. The CMOS serializer,
non-agent guards, pinned libslirp recipe and header fix each received separate
bounded reviews. Native test transcripts and source identities are in the
adjacent evidence files.

## Tree comparison and unpublished local fixes

After fetching each merge, these commands returned exit code zero (empty diff):

```text
git diff --exit-code dfb8cdb2e354d3c977a9c75a6e35ab88e7817ca2 44424e3ebe8ebc0c71bc1e060710979ebfb814b0
git diff --exit-code b7414809f4b810d6a4771a9f68f9baa2d4279ec8 a5f3c4e489a5cae7c10464adb6d85f218da589ac
git diff --exit-code 681d4ce 134d81488a4dacf9f300f1ddd66b86a996001e14
```

The last comparison covers the entire tree: merging the remote result into the
prepared local integration branch changed history only. The recorded 86-test
integration build and actual MSVC fallback probe therefore cover the same files.
See `cmos-local-integration-20260922.txt` for that run's transcript.

The integration preserves local commits `9801ce9` (memory-change run-until watches)
and `7832299` (transactional queued register writes). Comparing these three files
against original local pin `7832299e9cf32e61c4d329ea67899d2daf3d2505` also returned
an empty diff:

```text
docs/DEBUGGER_AGENT_API.md
src/agent/server/agent_server.cpp
tests/agent/agent_architecture_tests.cpp
```

Those are the only production/API/test differences from remote merge `a5f3c4e`;
the other integration-only files are this note and the native integration evidence.
No live emulator binary or checkpoint was replaced during these operations.
