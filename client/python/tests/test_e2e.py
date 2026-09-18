from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import time

CLIENT_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(CLIENT_ROOT))

from dosbox_agent import (
    AgentClient,
    BreakpointCondition,
    BreakpointHitFilter,
    KeyboardEvent,
    MemoryAddress,
    RegisterPreconditionFailedError,
    RunUntilPredicate,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True)
    arguments = parser.parse_args()
    config_path = Path(arguments.config).resolve()
    dosbox_config_path = config_path.with_suffix(".conf")
    client = AgentClient.from_config(config_path)
    if not client.config.dosbox_executable.is_file():
        raise RuntimeError(f"DOSBox-X executable was not found: {client.config.dosbox_executable}")
    if not dosbox_config_path.is_file():
        raise RuntimeError(f"DOSBox-X config was not found: {dosbox_config_path}")

    process = subprocess.Popen(
        [
            str(client.config.dosbox_executable),
            "-conf", str(dosbox_config_path),
            "-nopromptfolder",
            "--agent-config", str(config_path),
        ],
        cwd=REPOSITORY_ROOT,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    session_id: str | None = None
    try:
        # Popen returns before DOSBox-X has completed its shell bootstrap. This
        # is a process-readiness wait, not a retry of any stateful RPC.
        time.sleep(2)
        session = client.start("AGENTFIX.COM")
        session_id = session.id
        if session.state != "stopped" or session.stop_reason is None or session.stop_reason.kind != "startup":
            raise AssertionError("session.start did not stop at the fixture entry point")
        if session.stop_reason.address is None:
            raise AssertionError("session.start did not return the entry address")

        dos_map = client.get_dos_memory_map(session.id)
        if (dos_map.target.format != "com" or dos_map.target.image_bytes != 25 or
                dos_map.target.psp != session.stop_reason.address.segment or
                int(dos_map.target.load_segment, 16) != int(dos_map.target.psp, 16) + 0x10 or
                dos_map.target.entry_segment != session.stop_reason.address.segment or
                dos_map.target.entry_offset != "0x0100"):
            raise AssertionError(f"DOS loader metadata mismatch: {dos_map.target}")
        target_blocks = [block for block in dos_map.blocks if block.target_owned]
        process_blocks = [block for block in target_blocks if block.process]
        if (not process_blocks or process_blocks[0].data_segment != dos_map.target.psp or
                process_blocks[0].owner_psp != dos_map.target.psp or
                process_blocks[0].bytes != process_blocks[0].paragraphs * 16):
            raise AssertionError(f"DOS MCB ownership mismatch: {target_blocks}")
        print(
            "DOS-MAP evidence: "
            f"PSP={dos_map.target.psp} LOAD={dos_map.target.load_segment} "
            f"ENTRY={dos_map.target.entry_segment}:{dos_map.target.entry_offset} "
            f"image={dos_map.target.image_bytes} bytes target_blocks={len(target_blocks)}."
        )

        entry_registers = client.get_registers(session.id)
        old_eax = int(entry_registers.general["eax"], 16)
        old_ebx = int(entry_registers.general["ebx"], 16)
        new_eax = old_eax ^ 0xA5A55A5A
        new_ebx = old_ebx ^ 0x5A5AA5A5
        written_registers = client.set_registers(
            session.id,
            entry_registers.state_revision,
            {"eax": old_eax, "ebx": old_ebx},
            {"eax": new_eax, "ebx": new_ebx},
        )
        if (int(written_registers.before.general["eax"], 16) != old_eax or
                int(written_registers.before.general["ebx"], 16) != old_ebx or
                int(written_registers.after.general["eax"], 16) != new_eax or
                int(written_registers.after.general["ebx"], 16) != new_ebx or
                written_registers.after.state_revision !=
                written_registers.before.state_revision + 1):
            raise AssertionError(f"atomic register write mismatch: {written_registers}")
        try:
            client.set_registers(
                session.id,
                entry_registers.state_revision,
                {"eax": new_eax},
                {"eax": old_eax},
            )
            raise AssertionError("stale state revision unexpectedly wrote registers")
        except RegisterPreconditionFailedError:
            pass
        unchanged = client.get_registers(session.id)
        if (int(unchanged.general["eax"], 16) != new_eax or
                int(unchanged.general["ebx"], 16) != new_ebx):
            raise AssertionError("rejected register write partially changed live state")
        restored_registers = client.set_registers(
            session.id,
            unchanged.state_revision,
            {"eax": new_eax, "ebx": new_ebx},
            {"eax": old_eax, "ebx": old_ebx},
        )
        if (int(restored_registers.after.general["eax"], 16) != old_eax or
                int(restored_registers.after.general["ebx"], 16) != old_ebx):
            raise AssertionError("register restoration did not read back exactly")
        print(
            "REGISTER-WRITE evidence: EAX and EBX changed atomically with exact "
            "before/after snapshots; a stale revision was rejected with no partial write; "
            "both registers then restored exactly."
        )

        diagnostic = client.execute_command(session.id, "CPU")
        if not diagnostic.accepted or not diagnostic.raw_output:
            raise AssertionError("debugger.execute_command did not return diagnostic output")
        output = client.read_output(session.id, None, 1)
        if len(output.records) != 1 or output.records[0].sequence != 1 or not output.records[0].message:
            raise AssertionError("debug.output.read did not return the session-local diagnostic record")

        if not client.start_trace(session.id, "normal", 2):
            raise AssertionError("trace.start did not activate")
        trace_operation = client.continue_(session.id)
        trace_stop = client.wait(session.id, trace_operation.id, 10000)
        if trace_stop.running or trace_stop.session.state != "stopped":
            raise AssertionError("trace execution did not return to a stopped state")
        trace = client.read_trace(session.id, None, 2)
        if [event.sequence for event in trace.events] != [1, 2]:
            raise AssertionError("trace.read did not return exactly two session-local events")
        if any(event.effects for event in trace.events):
            raise AssertionError(f"register-only instructions reported effects: {trace.events}")
        trace_registers = client.get_registers(session.id)
        if trace_registers.instruction_pointer != "0x00000106":
            raise AssertionError(
                "bounded trace did not execute exactly two complete instructions: "
                f"{trace_registers.instruction_pointer}"
            )
        if client.stop_trace(session.id) != 2:
            raise AssertionError("trace.stop did not report two collected events")

        run_until = client.run_until(
            session.id,
            RunUntilPredicate(
                "execution",
                MemoryAddress.segmented(session.stop_reason.address.segment,
                                        "0x00000109"),
            ),
        )
        stopped = client.wait(session.id, run_until.id, 10000)
        reason = stopped.session.stop_reason
        if (stopped.running or reason is None or reason.kind != "run_until" or
                reason.breakpoint_id != run_until.predicate_id or reason.hit_count != 1):
            raise AssertionError(f"execution.run_until did not stop on its predicate: {reason}")
        if client.list_breakpoints(session.id):
            raise AssertionError("completed run-until predicate leaked into the breakpoint list")

        registers = client.get_registers(session.id)
        if registers.instruction_pointer != "0x00000109":
            raise AssertionError(f"unexpected run-until instruction pointer: {registers.instruction_pointer}")
        checkpoint = client.create_checkpoint(session.id, "before-memory-write")
        if (checkpoint.byte_count <= 0 or len(checkpoint.sha256) != 64 or
                client.list_checkpoints(session.id) != (checkpoint,)):
            raise AssertionError(f"checkpoint metadata mismatch: {checkpoint}")
        print(
            "RUN-UNTIL evidence: one RPC installed and resumed to "
            f"{session.stop_reason.address.segment}:0x00000109; "
            f"stop={reason.kind} predicate={reason.breakpoint_id} hit_count={reason.hit_count}; "
            "no temporary breakpoint remained."
        )

        code = client.read_memory(session.id, MemoryAddress.segmented(registers.segments["cs"], "0x00000100"), 6)
        if code.data != bytes((0xB8, 0x34, 0x12, 0xBB, 0x78, 0x56)):
            raise AssertionError(f"fixture code mismatch: {code.data.hex()}")

        data_address = MemoryAddress.segmented(registers.segments["ds"], "0x00000200")
        before = client.read_memory(session.id, data_address, 4)
        written = client.write_memory(
            session.id,
            data_address,
            b"\xDE\xAD\xBE\xEF",
            expected_sha256=hashlib.sha256(before.data).hexdigest(),
        )
        after = client.read_memory(session.id, data_address, 4)
        if after.data != b"\xDE\xAD\xBE\xEF" or written.after_sha256 != hashlib.sha256(after.data).hexdigest():
            raise AssertionError("memory.write verification failed")

        stepped, step_registers = client.step(session.id, "into")
        if stepped.stop_reason is None or stepped.stop_reason.kind != "step" or not step_registers.instruction_pointer:
            raise AssertionError("execution.step did not return a stopped register snapshot")

        restored_checkpoint, restored_session, restored_registers = client.restore_checkpoint(
            session.id, checkpoint.id
        )
        restored_data = client.read_memory(session.id, data_address, 4)
        if (restored_checkpoint != checkpoint or restored_session.stop_reason is None or
                restored_session.stop_reason.kind != "checkpoint_restore" or
                restored_registers.instruction_pointer != "0x00000109" or
                restored_data.data != before.data):
            raise AssertionError(
                "checkpoint did not restore the exact CPU/memory state: "
                f"checkpoint={restored_checkpoint} session={restored_session} "
                f"registers={restored_registers} data={restored_data.data.hex()}"
            )
        if not client.delete_checkpoint(session.id, checkpoint.id) or client.list_checkpoints(session.id):
            raise AssertionError("checkpoint delete did not empty the session checkpoint list")
        print(
            "CHECKPOINT evidence: "
            f"{checkpoint.id} retained {checkpoint.byte_count} bytes sha256={checkpoint.sha256}; "
            "after memory mutation and one step, restore returned IP=0x00000109 and the "
            f"original {len(before.data)} data bytes; delete left no checkpoints."
        )

        stop = client.stop(session.id)
        exited = client.wait(session.id, stop.id, 10000)
        if exited.running or exited.session.state != "exited" or exited.session.stop_reason is None or exited.session.stop_reason.kind != "session_stop":
            raise AssertionError("session.stop did not report controller termination")

        session = client.start("AGFX.COM")
        session_id = session.id
        if not client.start_hardware_trace(
            session.id, 1, include_irq=False, ports=((0x80, 0x80),)
        ):
            raise AssertionError("hardware I/O trace did not activate")
        if not client.start_trace(session.id, "normal", 6):
            raise AssertionError("effect trace did not activate")
        operation = client.continue_(session.id)
        stopped = client.wait(session.id, operation.id, 10000)
        if stopped.running or stopped.session.state != "stopped":
            raise AssertionError("effect trace did not complete at a stopped boundary")
        effect_trace = client.read_trace(session.id, None, 6)
        if len(effect_trace.events) != 6:
            raise AssertionError(f"effect trace returned {len(effect_trace.events)} events")
        effect_kinds = [effect.kind for event in effect_trace.events for effect in event.effects]
        if effect_kinds != ["memory_write", "memory_read", "io_write", "io_read"]:
            raise AssertionError(f"effect order mismatch: {effect_kinds}")
        write_effect = effect_trace.events[1].effects[0]
        read_effect = effect_trace.events[2].effects[0]
        io_write = effect_trace.events[4].effects[0]
        io_read = effect_trace.events[5].effects[0]
        expected_linear = (int(session.stop_reason.address.segment, 16) << 4) + 0x0113
        if (write_effect.address is None or int(write_effect.address.offset, 16) != expected_linear or
                write_effect.before != b"\x00\x00" or write_effect.after != b"\x34\x12"):
            raise AssertionError(f"memory-write trace evidence mismatch: {write_effect}")
        if (read_effect.address != write_effect.address or read_effect.data != b"\x34\x12"):
            raise AssertionError(f"memory-read trace evidence mismatch: {read_effect}")
        if io_write.port != "0x0080" or io_write.value != "0x00000034":
            raise AssertionError(f"I/O-write trace evidence mismatch: {io_write}")
        if io_read.port != "0x0080" or io_read.value is None:
            raise AssertionError(f"I/O-read trace evidence mismatch: {io_read}")
        effect_registers = client.get_registers(session.id)
        if (effect_registers.instruction_pointer != "0x0000010E" or
                int(effect_registers.general["eax"], 16) & 0xFF != int(io_read.value, 16) & 0xFF):
            raise AssertionError(
                f"I/O-read value did not become AL at the complete trace boundary: "
                f"registers={effect_registers} effect={io_read}"
            )
        if client.stop_trace(session.id) != 6:
            raise AssertionError("effect trace stop did not report six complete instructions")
        hardware_io = client.read_hardware_trace(session.id, None, 4)
        if (hardware_io.active is not True or hardware_io.capacity != 1 or
                hardware_io.dropped_event_count != 1 or
                hardware_io.first_available_sequence != 2 or
                len(hardware_io.events) != 1 or
                hardware_io.events[0].kind != "io_read" or
                hardware_io.events[0].phase != "instruction" or
                hardware_io.events[0].port != "0x0080" or
                hardware_io.events[0].address.offset != "0x0000010D"):
            raise AssertionError(f"bounded hardware I/O trace mismatch: {hardware_io}")
        if client.stop_hardware_trace(session.id).dropped_event_count != 1:
            raise AssertionError("hardware I/O trace stop lost overflow accounting")
        print(
            "HARDWARE-I/O evidence: capacity=1 retained sequence 2 io_read at "
            f"{hardware_io.events[0].address.segment}:0x0000010D; dropped=1."
        )
        operation = client.stop(session.id)
        client.wait(session.id, operation.id, 10000)

        session = client.start("AGINPUT.COM")
        session_id = session.id
        input_segment = session.stop_reason.address.segment
        if not client.start_hardware_trace(
            session.id, 16, include_io=False, irqs=(1,)
        ):
            raise AssertionError("hardware IRQ trace did not activate")
        pressed = client.send_keyboard(session.id, [
            KeyboardEvent("left_shift", True),
        ])
        if pressed.pressed_keys != ("left_shift",):
            raise AssertionError(f"device key-down state mismatch: {pressed}")
        shift_down = client.run_until(
            session.id,
            RunUntilPredicate("memory_write", MemoryAddress.segmented(input_segment, 0x0200)),
        )
        down_stop = client.wait(session.id, shift_down.id, 10000).session.stop_reason
        if (down_stop is None or down_stop.access is None or
                down_stop.access.after != b"\xD1"):
            raise AssertionError(f"guest did not observe left Shift make: {down_stop}")
        hardware_irq = client.read_hardware_trace(session.id, None, 16)
        irq_kinds = tuple(event.kind for event in hardware_irq.events)
        if ("irq_raise" not in irq_kinds or "irq_dispatch" not in irq_kinds or
                any(event.irq != 1 for event in hardware_irq.events) or
                any(hardware_irq.events[index].emulated_time_ns >
                    hardware_irq.events[index + 1].emulated_time_ns
                    for index in range(len(hardware_irq.events) - 1)) or
                next(event for event in hardware_irq.events
                     if event.kind == "irq_dispatch").vector != "0x0009"):
            raise AssertionError(f"hardware IRQ lifecycle mismatch: {hardware_irq}")
        print(
            "HARDWARE-IRQ evidence: physical Shift produced IRQ1 raise and dispatch "
            "to vector 0x09 with nondecreasing emulated timestamps."
        )

        released = client.send_keyboard(session.id, [
            KeyboardEvent("left_shift", False),
        ])
        if released.pressed_keys:
            raise AssertionError(f"device key release remained pressed: {released}")
        shift_up = client.run_until(
            session.id,
            RunUntilPredicate("memory_write", MemoryAddress.segmented(input_segment, 0x0201)),
        )
        up_stop = client.wait(session.id, shift_up.id, 10000).session.stop_reason
        if up_stop is None or up_stop.access is None or up_stop.access.after != b"\xD0":
            raise AssertionError(f"guest did not observe left Shift break: {up_stop}")
        client.stop_hardware_trace(session.id)

        joystick = client.set_joystick(
            session.id, 0, enabled=True, x=0, y=0,
            button0=True, button1=False,
        )
        if (not joystick.joysticks[0].enabled or
                joystick.joysticks[0].buttons != (True, False)):
            raise AssertionError(f"native joystick state mismatch: {joystick}")
        joystick_sample = client.run_until(
            session.id,
            RunUntilPredicate("memory_write", MemoryAddress.segmented(input_segment, 0x0202)),
        )
        joystick_stop = client.wait(session.id, joystick_sample.id, 10000).session.stop_reason
        if joystick_stop is None or joystick_stop.access is None:
            raise AssertionError(f"guest did not sample joystick port: {joystick_stop}")
        gameport = joystick_stop.access.after[0]
        if gameport & 0x10 or not gameport & 0x20:
            raise AssertionError(
                f"joystick buttons were not visible at port 0201h: 0x{gameport:02X}"
            )
        print(
            "INPUT evidence: native left-Shift make set BDA 0040:0017 bit 1 and its break "
            "cleared it; joystick 0 buttons [down,up] produced "
            f"port 0201h value 0x{gameport:02X}."
        )
        operation = client.stop(session.id)
        client.wait(session.id, operation.id, 10000)

        session = client.start("AGENTFIX.COM")
        session_id = session.id
        entry_registers = client.get_registers(session.id)
        data_address = MemoryAddress.segmented(entry_registers.segments["ds"], "0x00000200")
        original_data = client.read_memory(session.id, data_address, 3).data
        write_watch = client.create_watchpoint(
            session.id,
            "memory_write",
            MemoryAddress.segmented(entry_registers.segments["ds"], "0x00000201"),
            length=2,
            once=True,
        )
        if write_watch.length != 2 or not write_watch.once:
            raise AssertionError(f"memory-write watchpoint metadata mismatch: {write_watch}")
        operation = client.continue_(session.id)
        write_stop = client.wait(session.id, operation.id, 10000).session.stop_reason
        if (write_stop is None or write_stop.breakpoint_id != write_watch.id or
                write_stop.access is None or write_stop.hit_count != 1):
            raise AssertionError(f"memory-write watchpoint did not report typed access evidence: {write_stop}")
        expected_linear = (int(entry_registers.segments["ds"], 16) << 4) + 0x0201
        if (write_stop.access.kind != "write" or
                int(write_stop.access.address.offset, 16) != expected_linear or
                write_stop.access.before != original_data[1:3] or
                write_stop.access.after != b"CB" or
                write_stop.access.instruction_address.segment != entry_registers.segments["cs"] or
                write_stop.access.instruction_address.offset != "0x0000010C"):
            raise AssertionError(f"memory-write evidence mismatch: {write_stop.access}")
        if (write_stop.registers is None or
                write_stop.registers.phase != "after_instruction" or
                write_stop.registers.instruction_pointer != "0x00000111" or
                write_stop.registers.general["esi"] != "0x00000200"):
            raise AssertionError(f"memory-write post-instruction registers mismatch: {write_stop.registers}")

        read_watch = client.create_watchpoint(
            session.id,
            "memory_read",
            data_address,
            once=True,
        )
        operation = client.continue_(session.id)
        read_stop = client.wait(session.id, operation.id, 10000).session.stop_reason
        if (read_stop is None or read_stop.breakpoint_id != read_watch.id or
                read_stop.access is None or read_stop.hit_count != 1):
            raise AssertionError(f"memory-read watchpoint did not report typed access evidence: {read_stop}")
        if (read_stop.access.kind != "read" or
                read_stop.access.before != b"A" or
                read_stop.access.after != b"A" or
                read_stop.access.instruction_address.segment != entry_registers.segments["cs"] or
                read_stop.access.instruction_address.offset != "0x00000111"):
            raise AssertionError(f"memory-read evidence mismatch: {read_stop.access}")
        if (read_stop.registers is None or
                read_stop.registers.phase != "after_instruction" or
                read_stop.registers.instruction_pointer != "0x00000113" or
                int(read_stop.registers.general["eax"], 16) & 0xff != 0x41):
            raise AssertionError(f"memory-read post-instruction registers mismatch: {read_stop.registers}")
        if client.list_breakpoints(session.id):
            raise AssertionError("one-shot watchpoints remained after their matching accesses")
        print(
            "WATCHPOINT evidence: "
            f"write {write_stop.access.instruction_address.segment}:0x0000010C "
            f"linear={write_stop.access.address.offset} "
            f"old={write_stop.access.before.hex()} new={write_stop.access.after.hex()} post_ip=0x00000111; "
            f"read {read_stop.access.instruction_address.segment}:0x00000111 "
            f"value={read_stop.access.after.hex()} post_ip=0x00000113."
        )
        stop = client.stop(session.id)
        client.wait(session.id, stop.id, 10000)

        session = client.start("AGCOND.COM")
        session_id = session.id
        condition_breakpoint = client.create_execution_breakpoint(
            session.id,
            session.stop_reason.address.segment,
            "0x00000106",
            condition=BreakpointCondition("ax", "ne", 1),
            hit_filter=BreakpointHitFilter(skip=1, every=2),
        )
        filtered_values = []
        filtered_counts = []
        for expected_ax, expected_count in ((3, 2), (5, 4)):
            operation = client.continue_(session.id)
            reason = client.wait(session.id, operation.id, 10000).session.stop_reason
            if (reason is None or reason.breakpoint_id != condition_breakpoint.id):
                raise AssertionError(f"conditional breakpoint did not stop: {reason}")
            registers = client.get_registers(session.id)
            actual_ax = int(registers.general["eax"], 16) & 0xffff
            if actual_ax != expected_ax or reason.hit_count != expected_count:
                raise AssertionError(
                    f"conditional hit mismatch: AX={actual_ax}, hit_count={reason.hit_count}"
                )
            filtered_values.append(actual_ax)
            filtered_counts.append(reason.hit_count)
        listed = client.list_breakpoints(session.id)
        if listed != (condition_breakpoint,):
            raise AssertionError(f"conditional breakpoint metadata changed: {listed}")
        print(
            "BREAKPOINT-FILTER evidence: condition AX != 1, skip=1, every=2; "
            f"stops AX={filtered_values} at condition-hit counts {filtered_counts}."
        )
        stop = client.stop(session.id)
        client.wait(session.id, stop.id, 10000)

        session = client.start("AGINT.COM")
        session_id = session.id
        interrupt_breakpoint = client.create_interrupt_breakpoint(
            session.id, 0x21, ah=0x4C, once=True,
        )
        if client.list_breakpoints(session.id) != (interrupt_breakpoint,):
            raise AssertionError("software-interrupt selector changed while listed")
        operation = client.continue_(session.id)
        interrupt_stop = client.wait(session.id, operation.id, 10000).session.stop_reason
        if (interrupt_stop is None or
                interrupt_stop.breakpoint_id != interrupt_breakpoint.id or
                interrupt_stop.hit_count != 1 or interrupt_stop.event is None):
            raise AssertionError(
                f"software-interrupt breakpoint did not report typed evidence: {interrupt_stop}"
            )
        event = interrupt_stop.event
        if (event.number, event.ah, event.al, event.phase) != (0x21, 0x4C, 0x07, "before_handler"):
            raise AssertionError(f"software-interrupt event mismatch: {event}")
        interrupt_registers = client.get_registers(session.id)
        if int(interrupt_registers.general["eax"], 16) & 0xffff != 0x4C07:
            raise AssertionError(
                f"software-interrupt registers mismatch: {interrupt_registers.general['eax']}"
            )
        if client.list_breakpoints(session.id):
            raise AssertionError("one-shot software-interrupt breakpoint remained after its hit")
        operation = client.continue_(session.id)
        exited = client.wait(session.id, operation.id, 10000)
        reason = exited.session.stop_reason
        if (exited.running or exited.session.state != "exited" or reason is None or
                reason.kind != "program_exit" or reason.exit_code != 7):
            raise AssertionError(f"INT 21h termination did not run after breakpoint: {reason}")
        print(
            "INTERRUPT evidence: software INT 21h AH=4Ch AL=07h stopped "
            f"{event.phase}; AX={interrupt_registers.general['eax']}; DOS exit code={reason.exit_code}."
        )

        session = client.start("AGENTFIX.COM")
        session_id = session.id
        status = client.status(session.id)
        if status.target_psp is None:
            raise AssertionError("session.status did not report the target PSP")
        operation = client.continue_(session.id)
        exited = client.wait(session.id, operation.id, 10000)
        reason = exited.session.stop_reason
        if exited.running or exited.session.state != "exited" or reason is None or reason.kind != "program_exit":
            raise AssertionError(f"natural fixture exit was not reported as program_exit: {exited}")
        if reason.psp != status.target_psp or reason.exit_code != 0 or reason.tsr is not False:
            raise AssertionError(
                f"natural exit metadata mismatch: target PSP={status.target_psp}, stop reason={reason}"
            )

        session = client.start("COMMAND.COM", ("/C", "AGENTFIX.COM"))
        session_id = session.id
        status = client.status(session.id)
        if status.target_psp is None:
            raise AssertionError("COMMAND.COM session did not report the target PSP")
        operation = client.continue_(session.id)
        exited = client.wait(session.id, operation.id, 10000)
        reason = exited.session.stop_reason
        if exited.running or exited.session.state != "exited" or reason is None or reason.kind != "program_exit":
            raise AssertionError("COMMAND.COM did not survive its child process exit")
        if reason.psp != status.target_psp:
            raise AssertionError(
                f"child exit was mistaken for target exit: target PSP={status.target_psp}, stop reason={reason}"
            )

        race_kinds: set[str] = set()
        for _ in range(8):
            session = client.start("AGENTFIX.COM")
            session_id = session.id
            operation = client.continue_(session.id)
            stop = client.stop(session.id)
            if stop.id == "op-stop-complete":
                race_result = client.status(session.id)
            else:
                race_result = client.wait(session.id, stop.id, 10000).session
            if race_result.stop_reason is None or race_result.stop_reason.kind not in ("program_exit", "session_stop"):
                raise AssertionError(f"continue/stop race had invalid result: {race_result}")
            race_kinds.add(race_result.stop_reason.kind)

        print(
            "RPC-E02 passed: client operations, exact read/write watchpoints, conditional hit filters, controller stop, "
            "natural DOS exit, child-PSP filtering, "
            f"and 8 continue/stop races ({sorted(race_kinds)}) passed."
        )
        return 0
    finally:
        if session_id is not None:
            try:
                status = client.status(session_id)
                if status.state != "exited":
                    stop = client.stop(session_id)
                    client.wait(session_id, stop.id, 1000)
            except Exception:
                pass
        client.close()
        if process.poll() is None:
            process.kill()
        process.wait()


if __name__ == "__main__":
    raise SystemExit(main())
