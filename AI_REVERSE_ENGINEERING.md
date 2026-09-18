# AI-driven reverse-engineering fork

This public DOSBox-X fork is maintained on Jan Ryšavý's GitHub account to support
AI-driven dynamic reverse engineering of DOS software:

- fork: <https://github.com/janrysavy/dosbox-x-mpc>
- upstream: <https://github.com/joncampbell123/dosbox-x>
- project branch: `ai-re-agent`
- published under this name on the `janrysavy` account on 2026-09-18

The repository remains a true GitHub fork of upstream DOSBox-X. The requested public
repository identity was established by renaming the account's existing fork, which
preserves its fork ancestry. `ai-re-agent` is intentionally the default branch because
it contains the debugger-agent changes. Downstream repositories consume exact commits,
so published agent commits must remain available and must not be force-pushed.

## Focus

The fork turns DOSBox-X into a structured experimental instrument for autonomous and
human-guided reverse engineering. Its goals are exact observations, reproducible
transcripts, explicit machine state, bounded execution, and APIs that let an AI agent
stop, inspect, perturb, resume, and verify a DOS program without scraping an interactive
debugger window.

The fork adds a supervised JSON-RPC 2.0 debugger service over a local Windows named
pipe and a typed Python client. The implemented surface includes persistent sessions,
headless stepping and execution, exact breakpoints and watchpoints, register and memory
inspection and guarded mutation, atomic video snapshots, DOS loader maps, checkpoints,
authentic keyboard and joystick input, bounded CPU, hardware, and DOS-file traces,
emulated-time execution bounds, and structured target-exit reporting.

The repository name is `dosbox-x-mpc`. The current service is **not** a Model Context
Protocol (MCP) server and does not implement `tools/list`; it is custom JSON-RPC. A
future MCP adapter should remain thin and delegate debugger state and semantics to this
service.

The canonical command contract is
[docs/DEBUGGER_AGENT_API.md](docs/DEBUGGER_AGENT_API.md). It defines every supported
method, request and response shape, state rule, safety constraint, and evidence caveat.
The older [implementation guide](docs/rpc-agent-usage.md) provides additional examples,
but may lag the canonical contract. The server source and typed client remain
authoritative if documentation differs from the implementation.

## Building and testing

The primary Windows build is the MSVC Agent Debug SDL2 configuration. Run the debugger
agent tests non-interactively from this directory:

```cmd
python tests\agent\run_gtests.py
python -m unittest discover -s client\python\tests -p "test_*.py"
```

The upstream DOSBox-X licensing and attribution remain unchanged; see [COPYING](COPYING)
and [AUTHORS](AUTHORS).
