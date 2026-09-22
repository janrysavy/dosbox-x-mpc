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
