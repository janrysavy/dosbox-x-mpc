# Debugger agent API for AI agents

This is the entry point for an AI agent that needs to control the patched DOSBox-X.
It records the callable surface, the safe control loop, and the semantics that cannot
be inferred from a method name. It is deliberately shorter than the implementation
notes in the DOSBox-X fork.

## This is JSON-RPC, not a self-describing MCP server

The supported debugger is a JSON-RPC 2.0 service over a local Windows named pipe.
It does **not** implement the Model Context Protocol and does not answer MCP
`tools/list`. JSON-RPC itself has no standard method or schema discovery. The older
DOSBox-X facility named `debug_mcp` is a custom `REQ`/`BEGIN`/`END` line protocol;
its name does not make it a Model Context Protocol server either.

`agent.capabilities` is runtime feature discovery, not API discovery. It reports the
build's available facilities, accepted keys/address spaces, and numeric limits. It
does not return method names, descriptions, parameter schemas, or result schemas.

For this repository, an agent should use the typed Python client at
`client/python/dosbox_agent/`. Its method signatures validate
requests and its models validate responses. The raw JSON-RPC server is the final
authority when the client and documentation disagree. Run
`python scripts/check_debugger_agent_docs.py` after changing the server, client, or
this document; it requires the server method set, typed-client call set, and table
below to be identical, and requires every name to occur in the detailed section. It
does not validate parameter/result schemas or prose semantics; source review and tests
are still required for those.

The patched source is published as the public true DOSBox-X fork
[`janrysavy/dosbox-x-mpc`](https://github.com/janrysavy/dosbox-x-mpc), with
`ai-re-agent` as its default branch. The repository name does not change the protocol:
the implemented service remains the JSON-RPC surface documented here.

There are two standard ways to close this gap:

- Add OpenRPC service discovery to the existing JSON-RPC server. The OpenRPC
  specification reserves `rpc.discover` for returning a machine-readable document
  containing method descriptions plus parameter and result JSON Schemas.
- Add a real MCP adapter. An MCP server that advertises the `tools` capability must
  answer `tools/list`; each tool has a description and `inputSchema`, and should have
  an `outputSchema`. The adapter can pass the explicit `session_id` handle through to
  this stateful debugger service.

The recommended order is OpenRPC first, then generate the thin MCP adapter's tool
catalogue from that one schema. Do not independently hand-maintain 37 MCP schemas and
37 JSON-RPC schemas. The adapter must not acquire debugger semantics of its own.
Until it exists, do not tell an agent that this surface is self-describing.

Specifications:

- <https://spec.open-rpc.org/#service-discovery-method>
- <https://modelcontextprotocol.io/specification/2026-07-28/server/tools>

## Safe control loop

1. Start DOSBox-X with `--agent-config` and a unique named-pipe endpoint. Supervise
   the process, controlled scratch mount, provenance record, and timeout in the caller.
2. Call `AgentClient.capabilities()` and branch on returned features and limits.
3. Call `start(...)`. It returns only after the target is stopped at its entry point.
4. Inspect or modify stopped state. Start any trace that must observe the next run.
5. Call `continue_()` or `run_until()`. These return an operation immediately.
6. Repeatedly call `wait(operation_id, bounded_timeout_ms)`. A timeout means the
   operation is still running; it is not a failed run. Use `pause()` if control must
   be recovered.
7. On a stop, inspect `Session.stop_reason`, `state_revision`, registers, memory,
   traces, or a video snapshot. Do not infer a stop from the window.
8. If the target is still live, stop active recorders and drain their final pages. A
   natural program exit already stops the hardware and DOS recorders; after exit,
   read their final pages without calling stop again. Then call `stop()` only when
   controller cleanup is still needed and close the client. A `session_stop` is not
   proof that the DOS program exited.

Use a fresh, hash-checked scratch copy of the target for every run. Do not mount the
original program or data directory when the target can modify files beside itself.

## Complete callable inventory

The table is intentionally mechanical. The checker reads only rows between the two
inventory markers, so every server addition must gain a documented row and a typed
client call in the same slice.

<!-- BEGIN RPC METHOD INVENTORY -->
| JSON-RPC method | `AgentClient` entry point | Purpose and important contract |
| --- | --- | --- |
| `agent.capabilities` | `capabilities` | No session required. Returns runtime feature flags, supported values, and limits; it is not a method/schema catalogue. |
| `session.start` | `start` | Starts one target from the controlled C: mount and stops at its entry. Only one session may exist. |
| `session.status` | `status` | Returns state, revision, target identity, and the last stop when present. |
| `session.stop` | `stop` | Ends controller ownership. Its `session_stop` reason must never be reported as guest program exit. |
| `execution.continue` | `continue_` | Resumes a stopped target and returns an operation id. Follow it with `execution.wait`. |
| `execution.run_until` | `run_until` | Atomically installs one private one-shot predicate and resumes. Optional `max_emulated_ns` is an emulated-time instruction-boundary deadline. |
| `execution.pause` | `pause` | Requests a stop of a running target and returns an operation id. |
| `execution.step` | `step` | From a stopped target, performs `into` or `over` and returns the stopped session plus registers. |
| `execution.wait` | `wait` | Waits for one operation for a bounded host timeout. `running=true` means poll again; inspect the eventual structured stop reason. |
| `state.get_registers` | `get_registers` | Reads the canonical register snapshot and state revision while stopped. |
| `state.set_registers` | `set_registers` | Atomic guarded write. Supply the expected revision and old value for every full-width register being changed; mismatch rejects the entire write. |
| `input.keyboard` | `send_keyboard`, `send_key` | Applies an ordered batch of physical make/break events. Valid while stopped or running; accepted key names come from capabilities. |
| `input.joystick` | `set_joystick` | Updates native joystick 0 or 1; axis/button limits come from capabilities. |
| `input.state` | `get_input_state` | Reads exact currently pressed keys and joystick state. |
| `dos.memory_map` | `get_dos_memory_map` | While stopped, returns the target PSP/load metadata and DOS MCB ownership map. |
| `checkpoints.create` | `create_checkpoint` | Captures session-private emulator state while stopped. It does not snapshot mounted host files. |
| `checkpoints.list` | `list_checkpoints` | Lists retained checkpoint ids, labels, sizes, and hashes for the current session. |
| `checkpoints.restore` | `restore_checkpoint` | Restores a stopped checkpoint and returns a `checkpoint_restore` stop, registers, and a new revision. |
| `checkpoints.delete` | `delete_checkpoint` | Deletes one retained checkpoint; it does not affect guest or host files. |
| `video.snapshot` | `capture_video` | Atomically freezes text, fonts, palettes, CRTC/geometry, and the last renderer frame while stopped. The client then pages every component. |
| `video.snapshot.read` | `capture_video` (internal paging) | Reads an immutable snapshot component by id/offset/length. Prefer `capture_video`, which verifies counts and SHA-256 values. |
| `memory.read` | `read_memory` | Reads a bounded stopped-state block in segmented, linear, or physical space. The client validates base64 and length, but evidence code must verify the returned SHA-256 itself. |
| `memory.write` | `write_memory` | Writes bytes while stopped. Supply `expected_sha256` for compare-and-swap protection whenever prior contents matter. |
| `breakpoints.create` | `create_execution_breakpoint`, `create_watchpoint`, `create_interrupt_breakpoint`, `create_breakpoint` | Creates execution, exact access, memory-change, or semantic interrupt stops. Capability flags define available kinds/address spaces. |
| `breakpoints.list` | `list_breakpoints` | Returns public breakpoints only; a `run_until` predicate is private and is not listed. |
| `breakpoints.delete` | `delete_breakpoint` | Deletes one public breakpoint by id. One-shot and private predicates disappear automatically on their terminal stop. |
| `debug.output.read` | `read_output` | Pages bounded diagnostic output by cursor. Core evidence must use structured methods, not parsed debugger text. |
| `debugger.execute_command` | `execute_command` | Executes only the server's diagnostic allow-list. It is an escape hatch, not a substitute for structured state APIs. |
| `trace.start` | `start_trace` | Starts the bounded CPU instruction trace; requires a heavy-debug build and an advertised `trace.cpu` capability. |
| `trace.read` | `read_trace` | Pages CPU events by cursor. Events include address, instruction/register changes, ordered memory/I/O effects, and emulated time. |
| `trace.stop` | `stop_trace` | Freezes the CPU trace and reports its retained event count; drain it with `trace.read`. |
| `hardware.trace.start` | `start_hardware_trace` | Starts a bounded native port-I/O/IRQ ring with optional filters. It may run while the target runs. |
| `hardware.trace.read` | `read_hardware_trace` | Pages hardware events. Observe `dropped_event_count`, cursor expiry, and the returned emulated timestamps. |
| `hardware.trace.stop` | `stop_hardware_trace` | Freezes hardware recording without discarding retained events; perform a final read. |
| `dos.trace.start` | `start_dos_file_trace` | Starts a bounded target-PSP-filtered DOS create/open/read/write/seek/close recorder. It has no service/path filters; filter returned events in the caller. |
| `dos.trace.read` | `read_dos_file_trace` | Pages completed correlated DOS operations, including request and result fields, positions, exact counts, error status, payload hash, bounded preview, and drops. |
| `dos.trace.stop` | `stop_dos_file_trace` | Freezes DOS recording and returns final recorder metadata but not event pages; drain with `dos.trace.read`. |
<!-- END RPC METHOD INVENTORY -->

<!-- BEGIN RPC METHOD DETAILS -->
## Calling it from Python

The typed client is the preferred interface because it validates request shapes and
many response types, base64 encodings, and lengths. It is not a complete evidence
validator: important exceptions are called out below. A repository script can import
it without installing a package:

```python
import sys
from pathlib import Path

repo = Path(__file__).resolve().parents[1]  # adjust for the script's location
sys.path.insert(0, str(repo / "client/python"))

from dosbox_agent import (
    AgentClient, KeyboardEvent, MemoryAddress, RunUntilPredicate,
)

with AgentClient.from_config(repo / "reverse/configs/target.env") as agent:
    capabilities = agent.capabilities()
    # The config's dosbox_workdir is already the verified scratch mount. An explicit
    # mount_path is accepted only when it resolves to that exact same directory.
    session = agent.start("TARGET.EXE")
    registers = agent.get_registers(session.id)
    data = agent.read_memory(session.id, MemoryAddress.linear(0xB8000), 4000)
```

The public client signatures are below. `request_id=None` means the client allocates
one; supply an explicit run-unique id after reconnecting to an existing session.

```text
capabilities(request_id=None) -> Mapping
start(command, arguments=(), *, mount_path=None, request_id=None) -> Session
status(session_id, request_id=None) -> Session
stop(session_id, graceful_timeout_ms=0, request_id=None) -> Operation
continue_(session_id, request_id=None) -> Operation
run_until(session_id, predicate, max_emulated_ns=None, request_id=None) -> RunUntilOperation
pause(session_id, request_id=None) -> Operation
wait(session_id, operation_id, timeout_ms, request_id=None) -> WaitResult
step(session_id, mode="into", request_id=None) -> (Session, RegisterSnapshot)
get_registers(session_id, request_id=None) -> RegisterSnapshot
set_registers(session_id, expected_state_revision, expected, values, request_id=None) -> RegisterWriteResult
send_keyboard(session_id, events, request_id=None) -> InputState
send_key(session_id, key, pressed, request_id=None) -> InputState
set_joystick(session_id, index, *, enabled=None, x=None, y=None,
             button0=None, button1=None, request_id=None) -> InputState
get_input_state(session_id, request_id=None) -> InputState
get_dos_memory_map(session_id, request_id=None) -> DosMemoryMap
create_checkpoint(session_id, label="", request_id=None) -> Checkpoint
list_checkpoints(session_id, request_id=None) -> tuple[Checkpoint, ...]
restore_checkpoint(session_id, checkpoint_id, request_id=None) -> (Checkpoint, Session, RegisterSnapshot)
delete_checkpoint(session_id, checkpoint_id, request_id=None) -> bool
capture_video(session_id, request_id=None) -> VideoSnapshot
read_memory(session_id, address, length, request_id=None) -> MemoryRead
write_memory(session_id, address, data, *, expected_sha256=None, request_id=None) -> MemoryWrite
create_execution_breakpoint(session_id, segment, offset, *, once=False,
                            condition=None, hit_filter=BreakpointHitFilter(), request_id=None) -> Breakpoint
create_watchpoint(session_id, kind, address, length=1, *, once=False,
                  condition=None, hit_filter=BreakpointHitFilter(), request_id=None) -> Breakpoint
create_interrupt_breakpoint(session_id, number, *, ah=None, al=None, once=False,
                            condition=None, hit_filter=BreakpointHitFilter(), request_id=None) -> Breakpoint
create_breakpoint(session_id, kind, address, *, length=1, once=False,
                  condition=None, hit_filter=BreakpointHitFilter(), request_id=None) -> Breakpoint
list_breakpoints(session_id, request_id=None) -> tuple[Breakpoint, ...]
delete_breakpoint(session_id, breakpoint_id, request_id=None) -> int  # new state revision
read_output(session_id, cursor, limit, request_id=None) -> OutputPage
execute_command(session_id, command, request_id=None) -> DiagnosticCommandResult
start_trace(session_id, detail, instruction_count, request_id=None) -> bool
read_trace(session_id, cursor, limit, request_id=None) -> TracePage
stop_trace(session_id, request_id=None) -> int  # retained event count
start_hardware_trace(session_id, capacity, *, include_io=True, include_irq=True,
                     ports=(), irqs=(), request_id=None) -> bool
read_hardware_trace(session_id, cursor, limit, request_id=None) -> HardwareTracePage
stop_hardware_trace(session_id, request_id=None) -> HardwareTracePage
start_dos_file_trace(session_id, capacity, *, payload_preview_bytes=64, request_id=None) -> bool
read_dos_file_trace(session_id, cursor, limit, request_id=None) -> DosFileTracePage
stop_dos_file_trace(session_id, request_id=None) -> DosFileTracePage
```

`send_key` is a one-transition convenience wrapper over `input.keyboard`, not a key
press. A complete press normally requires make and break events, preferably in one
`send_keyboard` batch.

The client deliberately uses Python-native inputs rather than raw wire objects:
`send_keyboard` requires `KeyboardEvent` instances; `run_until` requires a
`RunUntilPredicate` (whose `address`, `condition`, `hit_filter`, or interrupt `event`
use the exported model classes); and `set_registers` requires mappings of names to
Python integers. Register snapshots contain hex strings, so convert them explicitly:

```python
before = agent.get_registers(session.id)
old_eax = int(before.general["eax"], 16)
agent.set_registers(
    session.id, before.state_revision,
    expected={"eax": old_eax}, values={"eax": (old_eax + 1) & 0xFFFFFFFF},
)
agent.send_keyboard(session.id, [
    KeyboardEvent("down", True), KeyboardEvent("down", False),
])
predicate = RunUntilPredicate(
    "execution", address=MemoryAddress.segmented(0x08AB, 0x28),
)
```

`AgentClient.close()` closes only the pipe transport; it does not stop the session or
the DOSBox-X process. The named-pipe server and DOSBox-X process must already be
running with the same configuration. A production caller should allocate a unique
pipe, refresh and hash-check a scratch copy, supervise the DOSBox-X process, record
requests and replies, impose deadlines, and own cleanup.

### Runtime capability result

`agent.capabilities` takes no parameters and returns the exact runtime choices an
agent must consult before constructing calls. The current nested keys are:

- `protocol_version` and `debugger`;
- `trace`: `cpu`, `memory_io_effects`, optional `emulated_timestamp_ns`, and
  `effects_require_normal_core`;
- `hardware_trace`: `io`, `irq`, `bounded`, `paged_read`,
  `emulated_timestamp_ns`, and `io_address_requires_normal_core`;
- `dos_file_trace`: `open_read_write_seek_close`, `target_psp_filter`, `bounded`,
  `paged_read`, `payload_sha256`, `payload_preview`, and
  `emulated_timestamp_ns`;
- `register_write`: guarded/atomic, stopped/revision/old-value requirements, and
  real-mode restriction;
- `execution`: `run_until`, `run_until_atomic`,
  `run_until_emulated_time_limit`, `stop_emulated_timestamp_ns`, and
  `run_until_predicate_kinds`;
- `input`: `device_keyboard`, `ordered_keyboard_batch`, `keyboard_state`, exact
  `keyboard_keys`, `joystick`, `joystick_count`, `joystick_axis_min`,
  `joystick_axis_max`, and `joystick_buttons`;
- `video`: `snapshot`, `atomic_components`, `paged_read`;
- `checkpoints`: `create`, `restore`, `session_memory`, `host_files`;
- `dos`: `memory_map`, `loader_metadata`;
- `breakpoints`: `software_interrupt`, `interrupt_phase`, `memory_change`,
  `memory_read`, `memory_write`, `memory_access`,
  `memory_change_address_spaces`, `exact_access_address_spaces`,
  `exact_access_requires_normal_core`, `condition_registers`,
  `condition_operators`, `conditional_kinds`, and `hit_filter`;
- `address_spaces`;
- `limits`: maximum message bytes, memory-read bytes, trace events, checkpoint count,
  and aggregate checkpoint bytes.

Never hard-code one machine's returned key list, address spaces, heavy-debug features,
or numeric limits into an investigation script.

## Raw wire contract

The transport is UTF-8 JSON Lines over the configured Windows named pipe: one complete
JSON-RPC request per line and one complete response per line. Requests use named
parameters only:

```json
{"jsonrpc":"2.0","id":"req-1","method":"memory.read","params":{"session_id":"ses-1","address":{"space":"linear","offset":"0x000B8000"},"length":4000}}
```

Success has `result`; failure has `error`, never both. Except for
`agent.capabilities`, successful session-bound results carry `session_id` and
`state_revision`. The service accepts one active session and serializes debugger
operations through the emulation thread. Do not send JSON-RPC batch arrays or
notifications: every debugger action needs an id and a checked response.

Request ids are session-scoped idempotency keys. The server caches the most recent
completed responses. Retry an ambiguously completed request only with the same id and
identical payload; reuse with a different payload returns `REQUEST_ID_CONFLICT`.

This rule applies to reads and polls too: every logically new observation needs a fresh
id. Reusing the id of an earlier `execution.wait` poll can replay `running:true`
forever, and reusing a trace-read id replays the old page. Reuse an id only to retransmit
the *same* request after an ambiguous transport failure. The cache is bounded to 1,024
responses and 8 MiB, may evict sooner under retained-video pressure, is cleared by
checkpoint restore, and disappears when a completed session is replaced. It therefore
provides bounded duplicate suppression, not durable exactly-once execution.

`AgentClient` generates `client-1`, `client-2`, ... and its counter restarts for every
new client object. Reconnecting a new client object to a retained session can therefore
collide with old ids. For a reconnect, pass explicit unique `request_id` values carrying
a run-specific prefix. Never use a timestamp alone where two controller processes could
produce the same value.

### Scalar and shared object forms

- Guest registers, selectors, addresses, returned ports, flags, handles, DOS-map PSPs,
  and interrupt values are fixed-width uppercase `0x` strings. Two lifecycle fields
  are exceptions: `session.status.target.psp` and `stop_reason.psp` are JSON integers.
  Hardware trace port-filter endpoints are also input as JSON integers. Use the exact
  representation specified for each field; do not send decimal guest addresses.
- Counts, lengths, sequence numbers, revisions, time values, and joystick axes are JSON
  integers.
- Binary bytes use RFC 4648 base64. SHA-256 values are lowercase 64-digit hex strings.
- `MemoryAddress` is one of
  `{"space":"segmented","segment":"0x1234","offset":"0x00000020"}`,
  `{"space":"linear","offset":"0x000B8000"}`, or
  `{"space":"physical","offset":"0x000B8000"}`. Supported spaces are returned by
  `agent.capabilities`.
- `BreakpointCondition` is
  `{"register":"ax","operator":"eq","value":"0x0000004C"}`. Registers and
  operators must come from capabilities.
- `HitFilter` is `{"skip":0,"every":1}`. It counts condition matches, skips the
  first `skip`, then selects encounter counts `skip+1`, `skip+1+every`, and so on.
  For `skip=2,every=3`, selections are cumulative hit counts 3, 6, 9, ... .
- `InterruptSelector` is
  `{"type":"software_interrupt","number":"0x21","ah":"0x4C","al":"0x00"}`;
  `ah` and `al` are optional.
- A non-interrupt breakpoint/predicate has `kind`, `address`, `length`, optional
  `condition`, and `hit_filter`. An interrupt form has `kind:"interrupt"`, `event`,
  optional `condition`, and `hit_filter`. Run-until predicates never accept `once`.

### Per-method raw contract index

This is the compact wire index. Brackets mean optional; `=` gives the raw-server
default unless explicitly identified as client-only. Dotted names describe nested
JSON fields. All session-bound success objects additionally include `session_id` and
`state_revision`; the family sections below define the named nested objects precisely.

| Method | Required params and optional/default fields | Method-specific success fields |
| --- | --- | --- |
| `agent.capabilities` | `{}` | capability objects and `limits` |
| `session.start` | `target.command`, `mounts`, `break_at="entry"`; `[target.arguments=[]]` | `state`, `stop_reason` |
| `session.status` | `session_id` | `state`, `target`, `[startup_diagnostic]`, `[last_stop]` |
| `session.stop` | `session_id`; `[graceful_timeout_ms=0, currently ignored]` | `operation_id` or non-waitable completed sentinel |
| `execution.continue` | `session_id` | `operation_id`, `state="running"` |
| `execution.run_until` | `session_id`, `predicate`; `[max_emulated_ns]` | `operation_id`, `predicate_id`, `state="running"` |
| `execution.pause` | `session_id` | `operation_id` |
| `execution.step` | `session_id`, `mode` (client default `"into"`) | `state`, `stop_reason`, `registers` |
| `execution.wait` | `session_id`, `operation_id`, `timeout_ms` | `running:true` while pending; otherwise `state`, `stop_reason` (no `running:false`) |
| `state.get_registers` | `session_id` | register snapshot fields |
| `state.set_registers` | `session_id`, `expected_state_revision`, `expected`, `set` | `before`, `after` |
| `input.keyboard` | `session_id`, `events` | `keyboard`, `joysticks` |
| `input.joystick` | `session_id`, `index`, at least one update field | `keyboard`, `joysticks` |
| `input.state` | `session_id` | `keyboard`, `joysticks` |
| `dos.memory_map` | `session_id` | `current_psp`, `first_mcb`, `target`, `blocks` |
| `checkpoints.create` | `session_id`; `[label=""]` | checkpoint descriptor |
| `checkpoints.list` | `session_id` | `checkpoints`, `retained_bytes` |
| `checkpoints.restore` | `session_id`, `checkpoint_id` | descriptor, `state`, `stop_reason`, registers |
| `checkpoints.delete` | `session_id`, `checkpoint_id` | `checkpoint_id`, `deleted` |
| `video.snapshot` | `session_id` | `snapshot_id`, capture metadata, six component descriptors |
| `video.snapshot.read` | `session_id`, `snapshot_id`, `component`, `offset`, `length` | chunk and whole-component metadata |
| `memory.read` | `session_id`, `address`, `length` | `address`, `byte_count`, `data_base64`, `sha256` |
| `memory.write` | `session_id`, `address`, `data_base64`; `[expected_sha256]` | `address`, `byte_count`, `before_sha256`, `after_sha256` |
| `breakpoints.create` | `session_id`, `kind`, address or interrupt event; `[length=1, once=false, condition, hit_filter={skip:0,every:1}]` | normalized breakpoint |
| `breakpoints.list` | `session_id` | `breakpoints` |
| `breakpoints.delete` | `session_id`, `breakpoint_id` | `breakpoint_id`, `deleted` |
| `debug.output.read` | `session_id`, `limit`; `[cursor=null]` | `output`, `next_cursor` |
| `debugger.execute_command` | `session_id`, `command` | `accepted`, `raw_output`, `parse_status`, optional parsed/unparsed fields |
| `trace.start` | `session_id`, `detail`, `instruction_count` | `active`, `detail`, `instruction_count` |
| `trace.read` | `session_id`, `limit`; `[cursor=null]` | `active`, `events`, `next_cursor` |
| `trace.stop` | `session_id` | `active=false`, `event_count` |
| `hardware.trace.start` | `session_id`, `capacity`; `[include_io=true, include_irq=true, ports=[], irqs=[]]` | `active`, `capacity` |
| `hardware.trace.read` | `session_id`, `limit`; `[cursor=null]` | recorder metadata, `events`, `next_cursor` |
| `hardware.trace.stop` | `session_id` | recorder metadata, `events=[]` |
| `dos.trace.start` | `session_id`, `capacity`; `[payload_preview_bytes=64]` | `active`, capacity/preview/target metadata |
| `dos.trace.read` | `session_id`, `limit`; `[cursor=null]` | recorder metadata, `events`, `next_cursor` |
| `dos.trace.stop` | `session_id` | recorder metadata, `events=[]` |

Here is one syntactically valid request shape for every raw method. Replace the sample
session/operation/checkpoint/snapshot/breakpoint ids, address, revision, and exact
configured mount path with values returned by the same run; whether a request is valid
*now* also depends on the state matrix below.

<!-- BEGIN RPC METHOD EXAMPLES -->
```jsonl
{"jsonrpc":"2.0","id":"ex-01","method":"agent.capabilities","params":{}}
{"jsonrpc":"2.0","id":"ex-02","method":"session.start","params":{"target":{"command":"TARGET.EXE","arguments":[]},"mounts":[{"drive":"C","host_path":"D:\\reverse\\target-run"}],"break_at":"entry"}}
{"jsonrpc":"2.0","id":"ex-03","method":"session.status","params":{"session_id":"ses-1"}}
{"jsonrpc":"2.0","id":"ex-04","method":"session.stop","params":{"session_id":"ses-1"}}
{"jsonrpc":"2.0","id":"ex-05","method":"execution.continue","params":{"session_id":"ses-1"}}
{"jsonrpc":"2.0","id":"ex-06","method":"execution.run_until","params":{"session_id":"ses-1","predicate":{"kind":"execution","address":{"space":"segmented","segment":"0x08AB","offset":"0x00000028"},"length":1,"hit_filter":{"skip":0,"every":1}}}}
{"jsonrpc":"2.0","id":"ex-07","method":"execution.pause","params":{"session_id":"ses-1"}}
{"jsonrpc":"2.0","id":"ex-08","method":"execution.step","params":{"session_id":"ses-1","mode":"into"}}
{"jsonrpc":"2.0","id":"ex-09","method":"execution.wait","params":{"session_id":"ses-1","operation_id":"op-1","timeout_ms":100}}
{"jsonrpc":"2.0","id":"ex-10","method":"state.get_registers","params":{"session_id":"ses-1"}}
{"jsonrpc":"2.0","id":"ex-11","method":"state.set_registers","params":{"session_id":"ses-1","expected_state_revision":12,"expected":{"eax":"0x00000000"},"set":{"eax":"0x00000001"}}}
{"jsonrpc":"2.0","id":"ex-12","method":"input.keyboard","params":{"session_id":"ses-1","events":[{"key":"down","pressed":true},{"key":"down","pressed":false}]}}
{"jsonrpc":"2.0","id":"ex-13","method":"input.joystick","params":{"session_id":"ses-1","index":0,"enabled":true,"x":0,"y":0}}
{"jsonrpc":"2.0","id":"ex-14","method":"input.state","params":{"session_id":"ses-1"}}
{"jsonrpc":"2.0","id":"ex-15","method":"dos.memory_map","params":{"session_id":"ses-1"}}
{"jsonrpc":"2.0","id":"ex-16","method":"checkpoints.create","params":{"session_id":"ses-1","label":"before-choice"}}
{"jsonrpc":"2.0","id":"ex-17","method":"checkpoints.list","params":{"session_id":"ses-1"}}
{"jsonrpc":"2.0","id":"ex-18","method":"checkpoints.restore","params":{"session_id":"ses-1","checkpoint_id":"checkpoint-1"}}
{"jsonrpc":"2.0","id":"ex-19","method":"checkpoints.delete","params":{"session_id":"ses-1","checkpoint_id":"checkpoint-1"}}
{"jsonrpc":"2.0","id":"ex-20","method":"video.snapshot","params":{"session_id":"ses-1"}}
{"jsonrpc":"2.0","id":"ex-21","method":"video.snapshot.read","params":{"session_id":"ses-1","snapshot_id":"snapshot-1","component":"text","offset":0,"length":4000}}
{"jsonrpc":"2.0","id":"ex-22","method":"memory.read","params":{"session_id":"ses-1","address":{"space":"linear","offset":"0x000B8000"},"length":4000}}
{"jsonrpc":"2.0","id":"ex-23","method":"memory.write","params":{"session_id":"ses-1","address":{"space":"linear","offset":"0x00024740"},"data_base64":"AA=="}}
{"jsonrpc":"2.0","id":"ex-24","method":"breakpoints.create","params":{"session_id":"ses-1","kind":"execution","address":{"space":"segmented","segment":"0x08AB","offset":"0x00000028"},"length":1,"once":true,"hit_filter":{"skip":0,"every":1}}}
{"jsonrpc":"2.0","id":"ex-25","method":"breakpoints.list","params":{"session_id":"ses-1"}}
{"jsonrpc":"2.0","id":"ex-26","method":"breakpoints.delete","params":{"session_id":"ses-1","breakpoint_id":"bp-1"}}
{"jsonrpc":"2.0","id":"ex-27","method":"debug.output.read","params":{"session_id":"ses-1","cursor":"out-0","limit":128}}
{"jsonrpc":"2.0","id":"ex-28","method":"debugger.execute_command","params":{"session_id":"ses-1","command":"HELP"}}
{"jsonrpc":"2.0","id":"ex-29","method":"trace.start","params":{"session_id":"ses-1","detail":"normal","instruction_count":256}}
{"jsonrpc":"2.0","id":"ex-30","method":"trace.read","params":{"session_id":"ses-1","cursor":"trace-0","limit":128}}
{"jsonrpc":"2.0","id":"ex-31","method":"trace.stop","params":{"session_id":"ses-1"}}
{"jsonrpc":"2.0","id":"ex-32","method":"hardware.trace.start","params":{"session_id":"ses-1","capacity":1024,"include_io":true,"include_irq":true,"ports":[],"irqs":[]}}
{"jsonrpc":"2.0","id":"ex-33","method":"hardware.trace.read","params":{"session_id":"ses-1","cursor":"hardware-0","limit":128}}
{"jsonrpc":"2.0","id":"ex-34","method":"hardware.trace.stop","params":{"session_id":"ses-1"}}
{"jsonrpc":"2.0","id":"ex-35","method":"dos.trace.start","params":{"session_id":"ses-1","capacity":1024,"payload_preview_bytes":64}}
{"jsonrpc":"2.0","id":"ex-36","method":"dos.trace.read","params":{"session_id":"ses-1","cursor":"dos-file-0","limit":128}}
{"jsonrpc":"2.0","id":"ex-37","method":"dos.trace.stop","params":{"session_id":"ses-1"}}
```
<!-- END RPC METHOD EXAMPLES -->

### Shared result schemas

Every session-bound result begins with
`session_id:string, state_revision:integer`. A register snapshot adds
`general:{eax,ebx,ecx,edx,esi,edi,ebp,esp}`, `segments:{cs,ds,es,fs,gs,ss}`,
`instruction_pointer`, `flags`, and `cpu_mode`; register values use the fixed-width
hex strings described above. `state.get_registers` returns these fields at the top
level, `execution.step` nests them under `registers`, and
`checkpoints.restore` returns them flat on the wire even though the Python wrapper
returns a separate `RegisterSnapshot` in its tuple.

`StopReason` always has `kind` and conditionally has these exact fields:

| Field | When present and exact shape |
| --- | --- |
| `emulated_time_ns` | Stop paths that captured the emulated clock. |
| `emulated_time_limit` | `{requested_duration_ns,start_emulated_time_ns,deadline_emulated_time_ns,actual_stop_emulated_time_ns,reached,overshoot_ns}` when run-until had a time budget. |
| `message` | Non-empty diagnostic/fault text. |
| `address` | Segmented or normalized breakpoint address for startup, step, breakpoint, run-until, time-limit, and checkpoint stops. |
| `breakpoint_id`, `hit_count` | Public `bp-*` or private `until-*` stop. `hit_count` is the cumulative condition-match count, including matches suppressed by the hit filter. |
| `event` | `{type:"software_interrupt",phase:"before_handler",number,ah,al}` for an interrupt match. |
| `access` | `{kind,address,byte_count,before_base64,after_base64,instruction_address}` for an exact memory access. Read events carry identical before/after bytes. |
| `registers` | Register fields plus `phase:"after_instruction"` and `state_revision` for exact-access stops. |
| `psp`, `exit_code`, `tsr` | Numeric PSP/code and boolean TSR flag only for `program_exit`. |

A CPU trace event is
`{sequence,emulated_time_ns,address,instruction,register_changes,effects}`. Each effect
has `kind` and `byte_count`, then exactly one of these payloads:

| Effect kind | Additional fields |
| --- | --- |
| `memory_read` | `address`, `data_base64` |
| `memory_write` | `address`, `before_base64`, `after_base64` |
| `io_read`, `io_write` | `port`, `value` |

A hardware event always has `sequence`, `emulated_time_ns`, `kind`, `address`, and
`phase`. `io_read|io_write` add `port`, `byte_count`, `value` and use
`phase:"instruction"`; `irq_raise|irq_lower` add numeric `irq` and use
`phase:"line"`; `irq_dispatch` adds numeric `irq`, hex `vector`, and uses
`phase:"before_handler"`.

A DOS event always emits all of these exact keys:
`sequence`, `correlation_id`, `emulated_time_ns`, `kind`, `target_psp`, `service`,
`caller_return_address`, `path`, `handle`, `system_handle`, `position_before`,
`position_after`, `requested_count`, `actual_count`, `requested_offset`,
`seek_origin`, `success`, `carry`, `error_code`, `payload_sha256`,
`payload_preview_base64`, and `payload_truncated`. `system_handle`, both positions,
the payload hash, and preview may be null. Non-applicable count/seek fields are zero;
interpret them according to `kind`, not as facts about another operation. On success,
`carry` is false and `error_code` is `0x0000`; on failure the payload fields are null.

A checkpoint descriptor is
`{checkpoint_id,label,captured_revision,byte_count,sha256}`. `dos.memory_map` returns
`current_psp`, `first_mcb`, optional
`target:{name,format,psp,load_segment,image_bytes,entry:{segment,offset},initial_stack:{segment,offset}}`,
and `blocks`. Every block has `mcb_segment`, `data_segment`, `paragraphs`, `bytes`,
`owner_psp`, `name`, `last`, `process`, and `target_owned`; process blocks additionally
have `parent_psp` and `environment_segment`.

`video.snapshot` returns `snapshot_id`, `captured_ticks`, `video_mode`, and:

| Object | Exact metadata beyond `{byte_count,sha256}` |
| --- | --- |
| `text` | `columns`, `glyph_height`, `start_offset` |
| `fonts` | `page_count`, `glyph_count`, `glyph_stride` |
| `dac` | `bits`, `pel_mask` |
| `renderer_palette` | none |
| `frame` | `kind`, `width`, `height`, `bpp`, `pitch`, `double_width`, `double_height` |
| `geometry` | `char9dot`, `blinking`, `blink_phase`, `attr_mode_control`, `underline_location`, `panning`, `draw_address`, and `crtc:{byte_count,sha256}` |

`video.snapshot.read` returns `snapshot_id`, `component`, `offset`, `byte_count`,
`data_base64`, chunk `sha256`, `component_byte_count`, `component_sha256`, and `eof`.
The trace-page schemas are: CPU `{active,events,next_cursor}`; hardware
`{active,capacity,dropped_event_count,first_available_sequence,events,next_cursor}`;
DOS adds `payload_preview_bytes` and `target_psp` to the hardware page fields.

### Session and execution methods

- `session.start` params are
  `target:{command:string,arguments?:string[]}`, exactly one
  `mounts:[{drive:"C",host_path:absolute_path}]`, and `break_at:"entry"`.
  `host_path` must resolve to the `dosbox_workdir` in `agent.env`; this is not a way to
  switch mounts. The command is one DOS filename token, not a path or shell command.
  Arguments allow alphanumerics, `.`, `_`, `-`, and an optional leading `/`; spaces,
  drive paths, and shell syntax are rejected. The result has session id/revision,
  `state:"stopped"`, and `stop_reason.kind:"startup"`; it does **not** include the
  PSP. Read the PSP from `session.status` or `dos.memory_map`. Start waits for the entry
  stop; it is not an operation.
- `session.status` takes `session_id` and returns `state`,
  `target:{command,[psp]}`, optional
  `startup_diagnostic:{phase,entry_breakpoint_created}` while starting, and optional
  `last_stop`.
- `session.stop` takes `session_id`. The Python wrapper also sends
  `graceful_timeout_ms`, but the current server does not read or enforce that field;
  do not treat it as a guest grace period. A live target returns an operation id to
  wait on. An already-exited target returns the sentinel `op-stop-complete`, which is
  not inserted in the operation table and must **not** be passed to `execution.wait`.
- `execution.continue` and `execution.pause` take `session_id` and return
  `operation_id`. Continue also reports `state:"running"`.
- `execution.run_until` takes `session_id`, one predicate, and optional positive
  64-bit `max_emulated_ns`. It returns `operation_id`, private `predicate_id`, and
  `state:"running"`. The private predicate is removed whether it wins or another stop
  wins.
- `execution.step` takes `session_id` and `mode:"into"|"over"`; it returns the
  stopped session, `stop_reason`, and a register snapshot.
- `execution.wait` takes `session_id`, `operation_id`, and unsigned 32-bit
  `timeout_ms`; zero is a valid nonblocking poll.
  While still executing it returns `running:true`; terminal results return state and
  `stop_reason` and omit `running`. Do not expect a stop reason in a running response.
  With `AgentClient`, keep `timeout_ms <= config.request_timeout_ms`; each transport
  request has only `request_timeout_ms + 1000` ms total. A larger server-side wait can
  outlive the pipe request and create an ambiguous transport timeout.

A stop reason always has `kind`; `emulated_time_ns` is present only when that stop path
captured it. Kinds are `startup`, `step`,
`breakpoint`, `run_until`, `checkpoint_restore`, `pause`, `program_exit`,
`session_stop`, `emulated_time_limit`, and `fault`. A terminal wait serializes the
session's latest stop rather than a historical stop stored inside the operation, so
consume and record a terminal result before continuing again. Depending on the kind it can also
contain `message`, `address`, `breakpoint_id` (also the private `until-*` id for a
run-until stop), `hit_count`, interrupt `event`, exact
memory `access`, post-instruction `registers`, `psp`, `exit_code`, `tsr`, and an
`emulated_time_limit` object. For a time limit that object reports requested duration,
start, deadline, actual stop time, `reached`, and overshoot, all in emulated
nanoseconds.

### Registers, memory, and input

- `state.get_registers` takes `session_id`. The result contains `general` (`eax`
  through `esp`), `segments` (`cs` through `ss`), `instruction_pointer`, `flags`,
  `cpu_mode`, and revision.
- `state.set_registers` takes `session_id`, `expected_state_revision`, non-empty
  `expected`, and non-empty `set`. Every name in `set` must be present in `expected`.
  Canonical writable names are `eax`, `ebx`, `ecx`, `edx`, `esi`, `edi`, `ebp`,
  `esp`, `cs`, `ds`, `es`, `fs`, `gs`, `ss`, `instruction_pointer`, and `flags`.
  General/IP/flags values are eight hex digits and segments are four. The result has
  complete `before` and `after` snapshots. The write is stopped-real-mode only and
  all-or-nothing.
- `memory.read` takes `session_id`, `address`, and positive `length` no larger than
  `limits.max_memory_read_bytes`; it returns the normalized address, `byte_count`,
  `data_base64`, `sha256`, and revision.
- `memory.write` takes `session_id`, `address`, non-empty `data_base64`, and optional
  64-hex-digit `expected_sha256` (the client normalizes it to lowercase). The decoded payload must contain
  `1..limits.max_memory_read_bytes` bytes. It rejects a mismatched precondition without
  writing and otherwise returns `byte_count`, `before_sha256`, `after_sha256`, and revision.
- `input.keyboard` takes `session_id` and 1..32 ordered
  `{"key":name,"pressed":boolean}` events. Names come from
  `capabilities.input.keyboard_keys`.
- `input.joystick` takes `session_id`, `index` 0 or 1, and at least one of `enabled`,
  signed `x`, signed `y`, `button0`, or `button1`.
- `input.state` takes `session_id`. All three input methods return `keyboard.pressed`
  and two ordered joystick objects containing `index`, `enabled`,
  `axes:{x,y}`, and `buttons:[button0,button1]`.

Input transitions are device events, not edits to the BIOS keyboard ring. They are
valid while stopped or running and exercise make/break traffic, BIOS flags, chords,
typematic repeat, and joystick port behavior.

### DOS map and checkpoints

- `dos.memory_map` takes `session_id`. It returns `current_psp`, `first_mcb`, a target
  with name, `com|mz` format, PSP, actual load segment and loaded image byte count,
  relocated `entry:{segment,offset}`, `initial_stack:{segment,offset}`, and `blocks`.
  Each block reports MCB/data segment, paragraphs/bytes, owner PSP, name, last/process
  flags, target ownership, and for process blocks parent PSP and environment segment.
- `checkpoints.create` takes `session_id` and an optional label of at most 64 UTF-8
  bytes. A descriptor has `checkpoint_id`, label, captured revision, serialized byte
  count, and SHA-256.
- `checkpoints.list` takes `session_id` and returns `checkpoints` plus
  `retained_bytes`.
- `checkpoints.restore` takes `session_id` and `checkpoint_id`; it returns the
  descriptor, stopped session, `checkpoint_restore` reason, and complete registers.
- `checkpoints.delete` takes `session_id` and `checkpoint_id`; it returns the id and
  `deleted:true`.

Create/restore require a stopped non-exited target and no active CPU, hardware, or DOS
trace recorder. List and delete also refuse a running target. Restoring invalidates
cached responses and video snapshots. Host files are never checkpointed or rolled
back.

### Atomic video snapshot

`video.snapshot` takes `session_id` and returns a `snapshot_id`, capture tick, video
mode, revision, and metadata for six immutable components: `text`, `fonts`, `dac`,
`renderer_palette`, `frame`, and `geometry.crtc`. Every component has `byte_count` and
`sha256`; text adds columns/glyph height/start offset, fonts add page/glyph counts and
stride, DAC adds bit depth and PEL mask, and frame adds kind, dimensions, bpp, pitch,
and double-width/height flags. Geometry also reports `char9dot`, blinking state/phase,
attribute mode control, underline location, panning, and draw address.

`video.snapshot.read` takes `session_id`, `snapshot_id`, one component name, `offset`,
and positive `length` not exceeding the memory-read limit. Component names are exactly
`text`, `fonts`, `dac`, `renderer_palette`, `frame`, and `crtc` (although the CRTC
metadata is nested at `geometry.crtc`). The requested range must fit completely; the
server never returns a short EOF read, so request
`min(max_memory_read_bytes, component_byte_count-offset)`. Empty components need no
read. It returns the chunk's
offset, count, base64, SHA-256 and EOF plus the entire component's count and SHA-256.
Prefer `AgentClient.capture_video()`: it pages all six components contiguously and
verifies their assembled sizes and whole-component hashes. It does not independently
verify each chunk's `sha256` or `eof` field.

The frame is the unscaled `renderer_source_cache`, not a PNG and not a host-window
screenshot. Snapshot ids are retained only as the current snapshot or while preserved
by the bounded response cache. A paged read's `state_revision` is the session revision
at read time; preserve the capture revision from the original snapshot response.
Text bytes are not necessarily ASCII because a DOS program can install its own VGA
glyphs. Interpret text, both font pages, palettes, frame, CRTC and normalized geometry
together. Component equality alone is insufficient when renderer geometry such as
`draw_address` changes how those components are presented.

### Breakpoints and run-until predicates

`breakpoints.create` accepts these kinds:

- `execution`: length must be 1; optional register condition; stops before execution.
- `interrupt`: semantic software `INT` selector with optional AH/AL and condition;
  direct calls to the interrupt vector do not match. The event is reported
  `phase:"before_handler"`.
- `memory_change`: length must be 1, `once` must remain false; legacy value-change
  stop.
- `memory_read`, `memory_write`, `memory_access`: positive exact-access range. These
  report the actual access address/width, old/new bytes where applicable, accessing
  CS:IP, and post-instruction registers.

`once` is optional and defaults false; `length` defaults 1; condition defaults absent;
`hit_filter` defaults to `{"skip":0,"every":1}`. Conditions are valid only for
execution and interrupt kinds. Execution addresses must be segmented. Exact access
addresses may be segmented or linear, never physical, and must resolve to one
contiguous range. Every length is bounded by `max_memory_read_bytes`.

Create returns a stable `breakpoint_id`, kind, once, normalized address/length or event,
normalized condition (null when absent), and hit filter. List adds `enabled`; delete
returns the id and `deleted:true`. Never treat a display-list index as an id. On an
exact-access stop, `access.address` and `access.byte_count` describe the actual CPU
access, not a range clipped to the watch. `access.instruction_address` is the accessing
instruction, while `stop_reason.registers.phase:"after_instruction"` is its post-state.

`execution.run_until` uses the same predicate shapes and evidence but is atomic,
private, and implicitly one-shot. Prefer it when the next action is immediately to
resume, because separate create/continue requests have a controller race.

`memory_change` run-until predicates are implemented as persistent native
breakpoints for the duration of the private operation, then deleted by the
normal stop cleanup after the first matching change. This is required because
the native adapter rejects one-shot `memory_change`; callers still must not send
`once` in the predicate. Use an exact `memory_write` predicate when the actual
write bytes are required.

### Diagnostic output and CPU trace

- `debug.output.read` takes `session_id`, nullable `cursor`, and positive `limit`.
  It returns `output` records (`sequence`, host timestamp, level, source, message) and
  nullable `next_cursor`. The ring retains 1,024 records; timestamps are host
  milliseconds and the cursor prefix is `out-`.
- `debugger.execute_command` takes `session_id` and an allow-listed diagnostic
  `command`. The complete allow-list is `HELP`, `CPU`, and `PIC`, case-insensitive
  after trimming; arguments are rejected. `CPU` is diagnostic text, not the canonical
  structured register dump. The result has `accepted`, `raw_output`, `parse_status`, and when applicable
  `parsed_registers` or `unparsed_output`. Never parse this instead of using a
  structured method.
- `trace.start` takes `session_id`, `detail:"short"|"normal"|"long"|"csip"`, and
  positive `instruction_count` within `limits.max_trace_events`. It returns `active`,
  detail, and count.
- `trace.read` takes `session_id`, nullable `cursor`, and positive `limit`. It returns
  `active`, ordered `events`, and nullable `next_cursor`.
- `trace.stop` takes `session_id` and returns `active:false` and `event_count`.

A CPU event identifies one guest instruction, but its timing must be read precisely:
address, timestamp, register snapshot and `register_changes` are captured **before**
that instruction executes; ordered `effects` accrue while it executes. Changes compare
successive pre-instruction samples, so the first event contains the full register
baseline and a later event's changes describe what happened since the previous sample.
After the count-limited trace stops, call `state.get_registers` for the final post-state.

Memory-read effects carry address/count/data; memory-write effects carry
address/count/before/after; I/O effects carry port/count/value. Counts are 1, 2, or 4.
Opcode/immediate fetches are not data-memory effects. `short` and `normal` currently
serialize the same instruction text; `long` appends debugger analysis; `csip` replaces
instruction text with the address. Reaching `instruction_count` automatically pauses
the target and normally completes the pending execution operation with stop kind
`pause`; there is no separate trace-complete stop kind.

When `trace.effects_require_normal_core` is true, launch DOSBox-X with `core=normal`
before relying on CPU memory/I/O effects. When
`hardware_trace.io_address_requires_normal_core` is true, the same requirement applies
to the CS:IP attributed to hardware I/O. Do not silently treat absent effects or an
unproven instruction address from another core as negative evidence.

### Hardware and DOS file trace rings

`hardware.trace.start` takes `session_id`, positive `capacity` no greater than
`max_trace_events`, optional `include_io` and `include_irq` (both default true),
optional `ports:[{first,last}]` with ordered 16-bit integer endpoints, and optional
`irqs` in 0..15. Empty filter arrays mean all ports/IRQs, not none. At least one event class must be enabled. Read takes a
nullable `hardware-N` cursor and `limit` in `1..limits.max_trace_events`. Events report sequence, emulated time,
kind, guest instruction address, phase, and either port/count/value or IRQ/vector.
Kinds are `io_read|io_write` with phase `instruction`, `irq_raise|irq_lower` with phase
`line`, and `irq_dispatch` with phase `before_handler` plus a vector.

`dos.trace.start` takes `session_id`, positive `capacity` no greater than
`max_trace_events`, and optional `payload_preview_bytes` in 0..4096 (default 64). It
records only the captured target PSP and has no service/path filter. Read uses
a nullable `dos-file-N` cursor and `limit` in `1..limits.max_trace_events`. Each event reports sequence,
correlation id, emulated time, completed-operation kind
`create|open|close|read|write|seek`, target PSP, DOS service, caller
return address, normalized path, DOS and optional system handles, positions,
requested/actual count, requested seek offset/origin, success/carry/error, full-payload
SHA-256, optional base64 preview, and truncation flag.

Services are AH `3C`, `3D`, `3E`, `3F`, `40`, and `42`, serialized in the 16-bit
service field as values such as `"0x003F"`. For successful reads/writes the payload
hash covers the **actual transferred bytes**, not the requested count. Failed transfers
have null hash and preview. A successful zero-byte transfer has SHA-256 of the empty
payload and an empty preview. Filter paths or services after reading the events.

Both read results report `active`, capacity, `dropped_event_count`,
`first_available_sequence`, events, and nullable `next_cursor`; DOS pages also report
preview size and target PSP. A cursor older than retained data returns
`CURSOR_EXPIRED`. Both stop methods freeze the recorder and return metadata with an
empty events array. Stop does not return the retained events: issue final paged reads.
If natural program exit already made `active:false`, do not issue another stop; directly
drain the frozen pages. A fresh repeated stop is not guaranteed idempotent.

### Cursor semantics and loss checks

A cursor means “the last sequence already consumed”; a read starts at `N+1`. Sequence
numbers begin at 1, so `trace-0`, `hardware-0`, `dos-file-0`, and `out-0` mean “before
the first event”. A missing or null cursor instead asks for the oldest event currently
retained. A cursor older than the ring or ahead of its newest sequence is expired.
Starting a new CPU/hardware/DOS recording clears that recorder and resets its sequence
epoch, so discard cursors from the previous recording.

`next_cursor:null` means no additional retained record remains after the last record
returned by this page at the instant of the read. The page itself can contain events.
It does **not** mean an active recorder has finished. For a live tail,
keep the last non-null cursor (or the sequence of the last returned event) and poll
again with that cursor. Passing null again restarts at the oldest retained event and
duplicates data. Every poll also needs a fresh JSON-RPC request id.

```python
# Live CPU tail. A production loop also has a wall-clock deadline.
cursor = "trace-0"
while True:
    page = agent.read_trace(session.id, cursor, limit=512)  # fresh request id
    for event in page.events:
        expected = int(cursor.split("-", 1)[1]) + 1
        if event.sequence != expected:
            raise RuntimeError(f"trace gap: expected {expected}, got {event.sequence}")
        cursor = f"trace-{event.sequence}"
        consume(event)
    if not page.active and not page.events:  # frozen recorder fully drained
        break
    if not page.events:
        poll_with_a_short_host_wait()
```

For hardware and DOS pages, fail or explicitly mark the evidence incomplete when
`dropped_event_count != 0`, and confirm that the first event follows the retained
cursor. For a frozen recorder, the same loop drains until a read returns no events.
For an active recorder, no-events means “caught up for now”, not EOF. The CPU page has
no dropped counter because its count-bounded trace stops instead of overwriting.

### Evidence validation missing from the typed client

Typed models reject many malformed shapes, but they do not make every server statement
true. In particular, `read_memory` checks base64 and the returned byte count but does
not verify the returned address, requested length, or SHA-256. Wrap evidence reads:

```python
import hashlib

def checked_read(agent, session_id, address, length):
    block = agent.read_memory(session_id, address, length)
    if block.address != address or len(block.data) != length:
        raise RuntimeError("memory.read returned a different range")
    digest = hashlib.sha256(block.data).hexdigest()
    if digest != block.sha256:
        raise RuntimeError("memory.read SHA-256 mismatch")
    return block
```

Likewise, check sequence continuity and drops for trace pages and validate DOS payload
hashes against the preview only when the preview is the complete payload
(`payload_truncated:false`). `capture_video` validates the assembled component length
and whole-component digest, but not individual chunk digests or `eof`. If those fields
are part of a claim, page `video.snapshot.read` explicitly and verify them too.

## Method-state and capability matrix

This table is normative for choosing when to call a method. `Any` means any retained
session state, including the final state where the server still has the session;
method-specific validation can still reject a nonsensical request.

| Methods | Allowed state | Additional prerequisite |
| --- | --- | --- |
| `agent.capabilities` | no session | none |
| `session.start` | no live session | controlled C: mount; `break_at=entry`; replaces a retained exited/failed session |
| `session.status`, `session.stop` | any | matching session id |
| `execution.continue`, `execution.run_until`, `execution.step` | stopped | requested breakpoint/trace capability must exist |
| `execution.pause` | running | none |
| `execution.wait` | any | matching operation id |
| `state.get_registers`, `state.set_registers`, `dos.memory_map` | stopped | register write is real-mode only; DOS map needs captured load metadata |
| `input.keyboard`, `input.joystick`, `input.state` | stopped or running | live, non-exited target |
| `checkpoints.create`, `checkpoints.restore` | stopped | no active CPU/hardware/DOS recorder; within limits |
| `checkpoints.list`, `checkpoints.delete` | any non-running retained session state | matching checkpoint id for delete |
| `video.snapshot`, `memory.read`, `memory.write` | stopped | address/size limits for memory |
| `video.snapshot.read` | retained snapshot | matching snapshot id/component and bounded range |
| `breakpoints.create`, `breakpoints.list`, `breakpoints.delete` | stopped | advertised kind/address space; normal core for exact access |
| `debug.output.read` | any | retained cursor or null |
| `debugger.execute_command` | stopped | non-empty allow-listed diagnostic command |
| `trace.start` | stopped | heavy-debug CPU trace; no active CPU trace |
| `trace.read`, `trace.stop` | any | a CPU trace has been started |
| `hardware.trace.start`, `dos.trace.start` | stopped or running | live target; DOS start also needs captured target PSP |
| `hardware.trace.read`, `dos.trace.read` | any | corresponding recorder has been started |
| `hardware.trace.stop`, `dos.trace.stop` | any | corresponding recorder exists and is active |

## Error contract

Standard JSON-RPC errors are `-32700` parse error, `-32600` invalid request,
`-32601` unknown method, `-32602` invalid params, and `-32603` internal error. Domain
errors put an uppercase `reason` in `error.data`; branch on that reason, not on the
human message or code alone. The table lists the normal code, but the mapping is not
one-to-one: some bridge failures use `COMMAND_REJECTED` with `-32004` rather than
`-32016`.

| Code | Reasons |
| --- | --- |
| `-32001` | `SESSION_BUSY` |
| `-32002` | `SESSION_NOT_FOUND` |
| `-32003` | `TARGET_RUNNING` |
| `-32004` | `CAPABILITY_UNAVAILABLE`, `TARGET_EXITED`, `TARGET_NOT_STOPPED`, `TARGET_NOT_RUNNING`, `TRACE_ACTIVE`, and some `COMMAND_REJECTED` paths |
| `-32009` | `REQUEST_TOO_LARGE`, `CHECKPOINT_LIMIT` |
| `-32010` | `REQUEST_ID_CONFLICT` |
| `-32011` | `OPERATION_TIMEOUT` |
| `-32012` | `INVALID_BINARY_LENGTH` |
| `-32013` | `MEMORY_PRECONDITION_FAILED` |
| `-32014` | `ADDRESS_NOT_MAPPED` |
| `-32015` | `BREAKPOINT_NOT_FOUND` |
| `-32016` | `COMMAND_REJECTED` |
| `-32017` | `CURSOR_EXPIRED` |
| `-32018` | `CHECKPOINT_NOT_FOUND` |
| `-32019` | `REGISTER_PRECONDITION_FAILED` |

`OPERATION_TIMEOUT` on a deferred adapter command means the server could not complete
that request within its configured RPC timeout; do not invent success. By contrast,
`execution.wait` returning `running:true` is a successful bounded poll and should be
repeated with a fresh request id. `AgentRpcError` always exposes `.code`, `.reason`,
and `.data`. The Python client has specific subclasses for most operational reasons,
but `TRACE_ACTIVE` and `CHECKPOINT_LIMIT` currently remain plain `AgentRpcError`.
<!-- END RPC METHOD DETAILS -->

## Contracts agents commonly get wrong

- **State is explicit.** Read/write/step/snapshot operations normally require a
  stopped target. Input and the bounded hardware/DOS recorders are designed to be
  usable while running. Trust a structured error over this summary if a boundary
  changes.
- **Operations are asynchronous.** `continue_`, `run_until`, and `pause` return an
  operation. Only `wait` supplies its terminal stop. Bound every wait by the
  controller's remaining wall-clock deadline.
- **Time domains differ.** `timeout_ms` on `wait` is a host wait. `max_emulated_ns`
  on `run_until` is a guest emulation deadline. Neither is a game-frame counter.
- **Revisions prevent stale writes.** Register writes require the stopped-state
  revision plus exact old values. Memory writes can require the old block's SHA-256.
- **Cursors and hashes are part of the evidence.** Preserve the last consumed
  sequence, reject expired cursors, report dropped events, and verify all declared
  lengths/hashes. The typed client verifies assembled video-component hashes, but a
  memory read needs the evidence wrapper above and trace pages need continuity checks.
- **Trace capacity is bounded.** CPU capture stops at its instruction count instead of
  overwriting. Hardware and DOS traces are overwriting diagnostic rings and report
  drops. None is a lossless long-gameplay recorder; do not interpret a gapped or
  overflowed capture as complete history.
- **Checkpoint scope excludes host files.** Restore rewinds emulator state but not
  `OPT`, `REC`, `HOF`, or any other mounted file. Fresh scratch runs remain the
  isolation boundary.
- **Natural exit has its own event.** A stop at `INT 21h/AH=4Ch` observes the request
  before DOS handles it. Proof of exit is the later PSP-matched `program_exit` event.
- **Request ids are idempotency keys.** Use a fresh id for every new poll/read/action.
  Only an exact retransmission after an ambiguous transport failure reuses an id.
  Reusing an id with a different payload is rejected; bounded cache eviction means an
  old id is not a permanent exactly-once guarantee.

## Reverse-engineering recipes

These are evidence recipes, not merely API demonstrations. Record every request and
response in the session transcript, including hashes, stop reasons, revisions, and
the scratch target's provenance.

### Relocate a static MZ address and prove that it is the same code

1. Start the target at entry and read `dos.memory_map`. Use
   `target.load_segment`; never assume the PSP or load segment from another run.
2. For a decoded image-relative `segment:offset`, compute runtime segment
   `load_segment + segment` (16-bit real-mode arithmetic) and retain the same
   offset. This rule is for addresses expressed relative to the MZ load module; do not
   apply it to an already-relocated selector or an absolute physical address.
3. `checked_read` the runtime bytes and compare them byte-for-byte with the pinned
   executable/disassembly window. A matching address without matching bytes is not a
   relocation proof.
4. Create a segmented execution breakpoint at the proven runtime address, continue,
   wait with bounded polls, and require the matching `breakpoint_id` and address in the
   stop reason. Read registers/arguments while stopped and tie the observation back to
   the decoded instruction.

### Find which instruction writes a global

1. Stop, read and hash the smallest known global range, then create a
   `memory_write` watchpoint for that exact contiguous range. Prefer `memory_write`
   over a persistent `memory_change` breakpoint unless legacy value-change semantics
   are specifically required.
2. Continue and wait. On the stop, inspect `stop_reason.access`: it contains the actual
   access address/width and before/after bytes, which can extend beyond the watched
   overlap. Require the expected transition rather than accepting any hit.
3. Use `access.instruction_address` and the post-instruction registers in the stop
   reason. Read a bounded code window around that address, decode it statically, and
   identify the write instruction and its address calculation. This gives the dynamic
   value a static home.
4. If the write can occur many times, use a condition/hit filter where supported or
   repeat with narrower state. Delete persistent public breakpoints when finished.

### Prove a routine's inputs, effects, and return state

1. Break at the byte-verified entry and record registers, relevant stack words, and
   globals before execution.
2. Start a bounded CPU trace, then continue or step. Trace event registers and address
   are pre-instruction; effects belong to that instruction. The first event is the
   full pre-state, not a change caused by the first instruction.
3. Stop at a proven return site or let the count limit pause execution. Drain the
   trace with continuity checks, then call `state.get_registers` for the final
   post-state. A trace's last pre-state is not the routine's return state.
4. Match observed reads/writes and control flow to decoded instructions. Record
   unresolved branches or inputs as debt instead of assigning plausible meanings.

### Drive input without confusing acceptance with consumption

1. Send physical transitions. A normal key press is a make plus break; `send_key`
   sends only one transition. Chords require ordered transitions.
2. `input.keyboard` success proves only that the emulator accepted the transition.
   It does not prove that the target consumed it. Run the CPU and stop on the target
   routine or state write that constitutes consumption, then verify the resulting
   memory or screen.
3. Use `input.state` to detect stuck keys. Release every pressed key during recovery,
   especially after a breakpoint interrupts a make/break sequence.

### Trace DOS file behavior exactly

1. Start `dos.trace` with enough capacity and an explicit preview size before the
   relevant execution. It automatically restricts events to the captured target PSP.
2. Drive the behavior and drain pages without gaps or drops. Each event is one
   completed create/open/close/read/write/seek operation, not separate entry/return
   records. Interpret actual counts, success/carry/error, positions, and full payload
   hash together; handles may be closed and reused.
3. Stop the recorder while the target is live, then drain it. If the program exited,
   it is already frozen: drain without calling stop. Filter paths/services in the
   controller because the server has no such start filters.

### Compare alternative executions with a checkpoint

1. Stop all recorders, create a labeled checkpoint, and record its descriptor plus
   baseline registers/memory/video hashes. Checkpoint creation refuses active
   recorders.
2. Apply one controlled input or memory mutation, run, and capture evidence.
3. Stop all recorders, restore, then reread revision, registers, memory, and video
   instead of reusing cached assumptions. Restore clears response/snapshot caches and
   changes the revision.
4. Mounted host files and debugger bookkeeping are not rolled back. Emulated
   timestamps can move backward after restore, so treat the restored run as a new
   trace/time epoch and use new cursors and request ids.

### Distinguish “asked DOS to exit” from actual termination

An interrupt breakpoint on `INT 21h/AH=4Ch` stops before the handler and proves only
that the program requested termination. Continue from it and require the later
PSP-matched `program_exit` stop containing the exit code/TSR status. `session_stop`
means controller cleanup and cannot substitute for that evidence.

### Keep one supervised session alive while reasoning

The caller owns the launch, unique pipe, refreshed and hash-checked scratch mount,
provenance, transcript, deadlines, and cleanup. Start once; alternate stopped
inspection, input, bounded resume/wait, and further inspection without rebooting.
Poll in short host waits and preserve every command and reply. On failure, release
input, pause a still-running target if possible, drain or freeze recorders according
to state, stop controller ownership, and ensure DOSBox-X cannot outlive the supervised
run.

## Where to read next

- [`../AI_REVERSE_ENGINEERING.md`](../AI_REVERSE_ENGINEERING.md) explains the fork's
  purpose, trust boundaries, and evidence model.
- `client/python/dosbox_agent/client.py` is the supported calling interface;
  `models.py` defines typed inputs and results.
- `src/agent/server/agent_server.cpp` is the final wire-contract authority.
- `docs/rpc-agent-usage.md` contains longer examples, but it is
  historical and may lag this inventory. Prefer this file and the typed client when
  they disagree.
