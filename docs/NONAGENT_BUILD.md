# Agent-only code in non-agent debugger builds

`DEBUG_HeavyIsBreakpoint` used agent trace, instruction-watch and emulated-time
limit symbols without the `C_DOSBOX_AGENT` guard that encloses their declarations.
At base `03bca583`, Release SDL2 x64 fails with 19 compiler errors in this routine.
The same source is present unchanged in CMOS PR #1; its inherited Visual Studio
and Linux checks exposed the problem independently of the CMOS change.

Guard the three agent-only blocks while retaining generic skip-first-instruction
and breakpoint behavior in every configuration. No statements change when the
agent is enabled. The focused CI matrix builds both Release SDL2 and Agent Debug
SDL2; inherited workflows remain enabled.

Local verification restored the exact base source for a failing compile, then
restored the six-line guard patch. Both builds passed using VS Insiders and
explicit v145 (the local machine lacks the Release dependencies' v142 default).
The source identities, 19 errors, build commands, isolated outputs, executable
hashes and native test run are retained in
[`nonagent-build-20260922.txt`](../tests/agent/evidence/nonagent-build-20260922.txt).
This establishes configuration coverage; it makes no snapshot completeness claim.

Independent review found the guard semantics sound, but requested exact Agent
build/test command binding and an explicit CI toolset. The evidence now records
the full Agent build launcher and a rerun of81 tests with an explicit executable
path and matching hash. The focused matrix selects windows-2025-vs2026/v145,
already used by the inherited VS2026 workflow; hosted success remains a CI gate.

## Legacy MSVC header follow-up

Final prerequisite CI exposed a second inherited compile defect: MSVC14.16
rejects `std::toupper` in `debug_mcp.cpp:822` because `<cctype>` was never
included. The source matches remote base03bca583 exactly. Add its direct
standard header; keep the existing unsigned-char cast and conversion behavior.
The isolated modern build and81 native tests pass; the legacy hosted build must
still confirm the fix. Source identity, failingCI link, exact build/test commands
and executable hash are in
[`mcp-cctype-20260922.txt`](../tests/agent/evidence/mcp-cctype-20260922.txt).

## Debug build without SDL networking (Win9x)

The retained Win9x job links a debug build with `--disable-sdlnet` (see
`build-mingw-lowend9x`). That selects the fallback block in `debug_mcp.cpp`,
which lacked `ControlServer_StartStdio` despite `DEBUG_Init` referencing it.
Job 106770092621 failed with that unresolved symbol. The missing definition is
also present in remote base `03bca583`; the preceding header fix did not cause it.
Add the same no-op fallback used by the other disabled channel entry points.
This fixes linking; it does not implement stdio transport without SDL networking.

`python tests/agent/test_mcp_disabled_link.py --compiler clang++` compiles the
production translation unit, links calls to its fallback APIs and executes the
result in debug/no-network, release/no-network and release/network configurations.
Both local Clang/MSVC and MinGW64 GCC pass all three. The unchanged probe fails
with the prior source's missing symbol under both compilers. Exact commands,
source identity, hashes and output are retained in
[`mcp-disabled-link-20260922.txt`](../tests/agent/evidence/mcp-disabled-link-20260922.txt).
The focused build matrix runs the probe; the complete Win9x hosted build is
still a required gate. No full Win9x success is claimed by this local probe.

A workflow can remain `in_progress` after a child job fails. Monitor individual
job conclusions as well as overall runs; `gh api repos/OWNER/REPO/actions/jobs/ID/logs`
retrieves completed-job logs while `gh run view --log` still refuses them.
