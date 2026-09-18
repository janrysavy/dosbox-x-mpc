from __future__ import annotations

import base64
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from dosbox_agent import (
    AddressNotMappedError,
    AgentClient,
    AgentConfig,
    AgentProtocolError,
    BreakpointNotFoundError,
    InvalidBinaryLengthError,
    MemoryAddress,
    MemoryPreconditionFailedError,
    RequestTooLargeError,
    SessionNotFoundError,
    TargetExitedError,
    TargetNotStoppedError,
    TargetRunningError,
)
from dosbox_agent.errors import map_rpc_error


class FakeTransport:
    def __init__(self, handler):
        self.handler = handler
        self.requests: list[dict] = []
        self.closed = False

    def request(self, payload: str, timeout_ms: int, max_message_bytes: int) -> str:
        request = json.loads(payload)
        self.requests.append(request)
        response = self.handler(request)
        return json.dumps({"jsonrpc": "2.0", "id": request["id"], **response})

    def close(self) -> None:
        self.closed = True


def make_config() -> AgentConfig:
    root = Path(__file__).resolve().parents[3]
    return AgentConfig(
        path=root / "tests" / "agent" / "agent-test.env",
        transport="named_pipe",
        endpoint=r"\\.\pipe\dosbox-agent-unit-test",
        dosbox_executable=root / "tests" / "agent" / "runtime" / "dosbox-x.exe",
        dosbox_workdir=root / "tests" / "agent" / "runtime",
        request_timeout_ms=5000,
        max_message_bytes=1024 * 1024,
        max_memory_read_bytes=65536,
        max_trace_events=10000,
        test_profile=True,
    )


def registers_result(revision: int = 3) -> dict:
    return {
        "session_id": "ses-1",
        "state_revision": revision,
        "general": {name: "0x00000000" for name in ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp")},
        "segments": {name: "0x0812" for name in ("cs", "ds", "es", "fs", "gs", "ss")},
        "instruction_pointer": "0x00000106",
        "flags": "0x00000202",
        "cpu_mode": "real",
    }


class AgentClientTests(unittest.TestCase):
    def test_all_v1_business_errors_have_typed_mappings(self) -> None:
        cases = {
            "SESSION_NOT_FOUND": SessionNotFoundError,
            "TARGET_NOT_STOPPED": TargetNotStoppedError,
            "TARGET_EXITED": TargetExitedError,
            "REQUEST_TOO_LARGE": RequestTooLargeError,
            "INVALID_BINARY_LENGTH": InvalidBinaryLengthError,
            "MEMORY_PRECONDITION_FAILED": MemoryPreconditionFailedError,
            "ADDRESS_NOT_MAPPED": AddressNotMappedError,
            "BREAKPOINT_NOT_FOUND": BreakpointNotFoundError,
        }
        for reason, error_type in cases.items():
            with self.subTest(reason=reason):
                error = map_rpc_error({"code": -32000, "message": reason, "data": {"reason": reason}})
                self.assertIsInstance(error, error_type)

    def test_config_is_explicit_and_relative_paths_resolve_from_config(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "agent.env"
            config.write_text(
                "\n".join((
                    "transport=named_pipe",
                    r"endpoint=\\.\pipe\client-test",
                    "dosbox_executable=runtime\\dosbox-x.exe",
                    "dosbox_workdir=runtime",
                    "profile=test",
                    "request_timeout_ms=50",
                    "max_message_bytes=64",
                    "max_memory_read_bytes=32",
                    "max_trace_events=16",
                )),
                encoding="utf-8",
            )
            client = AgentClient.from_config(config)
            self.assertTrue(client.config.test_profile)
            self.assertEqual((config.parent / "runtime" / "dosbox-x.exe").resolve(), client.config.dosbox_executable)
            self.assertEqual((config.parent / "runtime").resolve(), client.config.dosbox_workdir)
            client.close()

    def test_production_config_requires_absolute_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "agent.env"
            config.write_text(
                "\n".join((
                    "transport=named_pipe",
                    r"endpoint=\\.\pipe\client-test",
                    "dosbox_executable=runtime\\dosbox-x.exe",
                    "dosbox_workdir=runtime",
                    "request_timeout_ms=50",
                    "max_message_bytes=64",
                    "max_memory_read_bytes=32",
                    "max_trace_events=16",
                )),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "absolute"):
                AgentClient.from_config(config)

    def test_typed_methods_cover_v1_contract(self) -> None:
        address = {"space": "segmented", "segment": "0x0812", "offset": "0x00000106"}
        video_components = {
            "text": b"A\x1f", "fonts": b"font", "dac": b"dac",
            "renderer_palette": b"rgb", "frame": b"frame", "crtc": b"crtc",
        }

        def video_metadata(component: str) -> dict:
            data = video_components[component]
            return {"byte_count": len(data), "sha256": hashlib.sha256(data).hexdigest()}

        def handler(request: dict) -> dict:
            method = request["method"]
            if method == "agent.capabilities":
                return {"result": {"protocol_version": "1.0"}}
            if method == "session.start":
                return {"result": {"session_id": "ses-1", "state_revision": 1, "state": "stopped", "stop_reason": {"kind": "startup", "address": address}}}
            if method == "session.status":
                return {"result": {"session_id": "ses-1", "state_revision": 1, "state": "stopped", "last_stop": {"kind": "startup", "address": address}}}
            if method in ("execution.continue", "execution.pause", "session.stop"):
                return {"result": {"session_id": "ses-1", "state_revision": 2, "operation_id": "op-1"}}
            if method == "execution.wait":
                return {"result": {"session_id": "ses-1", "state_revision": 2, "state": "stopped", "stop_reason": {"kind": "breakpoint", "address": address}}}
            if method == "execution.step":
                result = registers_result(4)
                result["stop_reason"] = {"kind": "step", "address": address}
                result["registers"] = registers_result(4)
                return {"result": result}
            if method == "state.get_registers":
                return {"result": registers_result()}
            if method == "video.snapshot":
                text = {**video_metadata("text"), "columns": 80, "glyph_height": 16, "start_offset": 0}
                fonts = {**video_metadata("fonts"), "page_count": 2, "glyph_count": 256, "glyph_stride": 32}
                dac = {**video_metadata("dac"), "bits": 6, "pel_mask": 255}
                frame = {
                    **video_metadata("frame"), "kind": "renderer_source_cache", "width": 2, "height": 1,
                    "bpp": 8, "pitch": 2, "double_width": False, "double_height": False,
                }
                geometry = {"char9dot": True, "crtc": video_metadata("crtc")}
                return {"result": {
                    "session_id": "ses-1", "state_revision": 3, "captured_ticks": 42,
                    "snapshot_id": "shot-1", "video_mode": 3, "text": text, "fonts": fonts, "dac": dac,
                    "renderer_palette": video_metadata("renderer_palette"), "frame": frame, "geometry": geometry,
                }}
            if method == "video.snapshot.read":
                params = request["params"]
                data = video_components[params["component"]]
                chunk = data[params["offset"]:params["offset"] + params["length"]]
                return {"result": {
                    "session_id": "ses-1", "state_revision": 3, "snapshot_id": "shot-1",
                    "component": params["component"], "offset": params["offset"],
                    "byte_count": len(chunk), "data_base64": base64.b64encode(chunk).decode("ascii"),
                    "sha256": hashlib.sha256(chunk).hexdigest(), "component_byte_count": len(data),
                    "component_sha256": hashlib.sha256(data).hexdigest(), "eof": True,
                }}
            if method == "memory.read":
                return {"result": {"session_id": "ses-1", "state_revision": 3, "address": address, "byte_count": 3, "data_base64": "QUJD", "sha256": "a" * 64}}
            if method == "memory.write":
                return {"result": {"session_id": "ses-1", "state_revision": 4, "address": address, "byte_count": 3, "before_sha256": "b" * 64, "after_sha256": "c" * 64}}
            if method == "breakpoints.create":
                return {"result": {"session_id": "ses-1", "state_revision": 5, "breakpoint_id": "bp-1", "kind": "execution", "length": 1, "once": False, "address": address}}
            if method == "breakpoints.list":
                return {"result": {"session_id": "ses-1", "state_revision": 5, "breakpoints": [{"breakpoint_id": "bp-1", "kind": "execution", "length": 1, "enabled": True, "once": False, "address": address}]}}
            if method == "breakpoints.delete":
                return {"result": {"session_id": "ses-1", "state_revision": 6, "breakpoint_id": "bp-1", "deleted": True}}
            if method == "debug.output.read":
                return {"result": {"session_id": "ses-1", "state_revision": 6, "output": [{"sequence": 1, "timestamp": 1, "level": "info", "source": "debugger.command", "message": "CPU"}], "next_cursor": None}}
            if method == "debugger.execute_command":
                return {"result": {"session_id": "ses-1", "state_revision": 6, "accepted": True, "raw_output": "CPU", "parse_status": "failed", "unparsed_output": "CPU"}}
            if method == "trace.start":
                return {"result": {"session_id": "ses-1", "state_revision": 6, "active": True}}
            if method == "trace.read":
                return {"result": {"session_id": "ses-1", "state_revision": 6, "active": False, "events": [{"sequence": 1, "address": address, "instruction": "MOV AX,1234", "register_changes": {"eax": "0x00001234"}}], "next_cursor": None}}
            if method == "trace.stop":
                return {"result": {"session_id": "ses-1", "state_revision": 6, "event_count": 1}}
            self.fail(f"unexpected method {method}")

        transport = FakeTransport(handler)
        client = AgentClient(make_config(), transport)
        self.assertEqual("1.0", client.capabilities()["protocol_version"])
        session = client.start("AGENTFIX.COM")
        self.assertEqual("0x0812", session.stop_reason.address.segment)
        self.assertEqual("0x00000106", client.status(session.id).stop_reason.address.offset)
        self.assertEqual("op-1", client.continue_(session.id).id)
        self.assertFalse(client.wait(session.id, "op-1", 1).running)
        stepped, stepped_registers = client.step(session.id)
        self.assertEqual("stopped", stepped.state)
        self.assertEqual("real", stepped_registers.cpu_mode)
        self.assertEqual("0x00000106", client.get_registers(session.id).instruction_pointer)
        video = client.capture_video(session.id)
        self.assertEqual(b"A\x1f", video.text.data)
        self.assertEqual(80, video.text_columns)
        self.assertEqual(b"frame", video.frame.block.data)
        self.assertTrue(video.geometry["char9dot"])
        self.assertEqual(b"ABC", client.read_memory(session.id, MemoryAddress.segmented(0x812, 0x106), 3).data)
        self.assertEqual("c" * 64, client.write_memory(session.id, MemoryAddress.segmented(0x812, 0x106), b"ABC").after_sha256)
        breakpoint = client.create_execution_breakpoint(session.id, 0x812, 0x106)
        self.assertEqual("bp-1", breakpoint.id)
        self.assertEqual((breakpoint,), client.list_breakpoints(session.id))
        self.assertEqual(6, client.delete_breakpoint(session.id, breakpoint.id))
        self.assertEqual("CPU", client.read_output(session.id, None, 1).records[0].message)
        self.assertTrue(client.execute_command(session.id, "CPU").accepted)
        self.assertTrue(client.start_trace(session.id, "normal", 1))
        self.assertEqual("MOV AX,1234", client.read_trace(session.id, None, 1).events[0].instruction)
        self.assertEqual(1, client.stop_trace(session.id))
        self.assertEqual("op-1", client.pause(session.id).id)
        self.assertEqual("op-1", client.stop(session.id).id)
        self.assertEqual(26, len(transport.requests))
        self.assertEqual(str(make_config().dosbox_workdir), transport.requests[1]["params"]["mounts"][0]["host_path"])

    def test_watchpoint_request_and_stop_are_typed(self) -> None:
        watched_address = {"space": "segmented", "segment": "0x0812", "offset": "0x00000200"}
        access_address = {"space": "linear", "offset": "0x00008320"}
        instruction_address = {"space": "segmented", "segment": "0x0812", "offset": "0x00000109"}

        def handler(request: dict) -> dict:
            if request["method"] == "breakpoints.create":
                params = request["params"]
                return {"result": {
                    "session_id": "ses-1", "state_revision": 2, "breakpoint_id": "bp-watch",
                    "kind": params["kind"], "length": params["length"], "once": params["once"],
                    "address": params["address"],
                }}
            if request["method"] == "execution.wait":
                register_values = registers_result(3)
                register_values["phase"] = "after_instruction"
                return {"result": {
                    "session_id": "ses-1", "state_revision": 3, "state": "stopped",
                    "stop_reason": {
                        "kind": "breakpoint", "breakpoint_id": "bp-watch",
                        "address": watched_address,
                        "access": {
                            "kind": "write", "address": access_address, "byte_count": 2,
                            "before_base64": base64.b64encode(b"\x00\x00").decode("ascii"),
                            "after_base64": base64.b64encode(b"CB").decode("ascii"),
                            "instruction_address": instruction_address,
                        },
                        "registers": register_values,
                    },
                }}
            self.fail(f"unexpected method {request['method']}")

        transport = FakeTransport(handler)
        client = AgentClient(make_config(), transport)
        breakpoint = client.create_watchpoint(
            "ses-1", "memory_write", MemoryAddress.segmented(0x0812, 0x0200),
            length=3, once=True,
        )
        self.assertEqual("memory_write", breakpoint.kind)
        self.assertEqual(3, breakpoint.length)
        self.assertTrue(breakpoint.once)
        self.assertEqual(3, transport.requests[0]["params"]["length"])

        stopped = client.wait("ses-1", "op-1", 100).session.stop_reason
        self.assertIsNotNone(stopped)
        self.assertEqual("bp-watch", stopped.breakpoint_id)
        self.assertEqual("write", stopped.access.kind)
        self.assertEqual(b"\x00\x00", stopped.access.before)
        self.assertEqual(b"CB", stopped.access.after)
        self.assertEqual("0x00000109", stopped.access.instruction_address.offset)
        self.assertEqual("after_instruction", stopped.registers.phase)
        self.assertEqual(3, stopped.registers.state_revision)

    def test_watchpoint_stop_rejects_malformed_binary_evidence(self) -> None:
        address = {"space": "linear", "offset": "0x00008320"}
        instruction = {"space": "segmented", "segment": "0x0812", "offset": "0x00000109"}

        def handler(request: dict) -> dict:
            return {"result": {
                "session_id": "ses-1", "state_revision": 3, "state": "stopped",
                "stop_reason": {
                    "kind": "breakpoint",
                    "access": {
                        "kind": "write", "address": address, "byte_count": 2,
                        "before_base64": "not-base64", "after_base64": "Q0I=",
                        "instruction_address": instruction,
                    },
                },
            }}

        client = AgentClient(make_config(), FakeTransport(handler))
        with self.assertRaisesRegex(ValueError, "invalid base64"):
            client.wait("ses-1", "op-1", 100)

    def test_status_and_program_exit_preserve_process_lifecycle_metadata(self) -> None:
        def handler(request: dict) -> dict:
            if request["method"] == "session.status":
                return {"result": {
                    "session_id": "ses-1",
                    "state_revision": 3,
                    "state": "exited",
                    "target": {"command": "AGENTFIX.COM", "psp": 0x1234},
                    "last_stop": {
                        "kind": "program_exit",
                        "psp": 0x1234,
                        "exit_code": 7,
                        "tsr": False,
                    },
                }}
            self.fail(f"unexpected method {request['method']}")

        client = AgentClient(make_config(), FakeTransport(handler))
        session = client.status("ses-1")
        self.assertEqual(0x1234, session.target_psp)
        self.assertIsNotNone(session.stop_reason)
        self.assertEqual("program_exit", session.stop_reason.kind)
        self.assertEqual(0x1234, session.stop_reason.psp)
        self.assertEqual(7, session.stop_reason.exit_code)
        self.assertIs(False, session.stop_reason.tsr)

    def test_video_snapshot_rejects_a_binary_length_mismatch(self) -> None:
        def handler(request: dict) -> dict:
            metadata = {"byte_count": 1, "sha256": hashlib.sha256(b"A").hexdigest()}
            if request["method"] == "video.snapshot.read":
                return {"result": {
                    "session_id": "ses-1", "state_revision": 1, "snapshot_id": "shot-1",
                    "component": request["params"]["component"], "offset": 0,
                    "byte_count": 2, "data_base64": "QQ==", "sha256": hashlib.sha256(b"A").hexdigest(),
                    "component_byte_count": 1, "component_sha256": metadata["sha256"], "eof": True,
                }}
            self.assertEqual("video.snapshot", request["method"])
            return {"result": {
                "session_id": "ses-1", "state_revision": 1, "snapshot_id": "shot-1",
                "captured_ticks": 0, "video_mode": 3,
                "text": {**metadata, "columns": 80, "glyph_height": 16, "start_offset": 0},
                "fonts": {**metadata, "page_count": 2, "glyph_count": 256, "glyph_stride": 32},
                "dac": {**metadata, "bits": 6, "pel_mask": 255},
                "renderer_palette": metadata,
                "frame": {**metadata, "kind": "renderer_source_cache", "width": 1, "height": 1,
                          "bpp": 8, "pitch": 1, "double_width": False, "double_height": False},
                "geometry": {"crtc": metadata},
            }}

        client = AgentClient(make_config(), FakeTransport(handler))
        with self.assertRaisesRegex(AgentProtocolError, "byte_count"):
            client.capture_video("ses-1")

    def test_base64_request_error_mapping_and_explicit_request_id_retry(self) -> None:
        write_response = {"session_id": "ses-1", "state_revision": 2, "address": {"space": "segmented", "segment": "0x0812", "offset": "0x00000200"}, "byte_count": 2, "before_sha256": "a" * 64, "after_sha256": "b" * 64}

        def handler(request: dict) -> dict:
            if request["method"] == "state.get_registers":
                return {"error": {"code": -32012, "message": "Target is running", "data": {"reason": "TARGET_RUNNING"}}}
            return {"result": write_response}

        transport = FakeTransport(handler)
        client = AgentClient(make_config(), transport)
        with self.assertRaises(TargetRunningError):
            client.get_registers("ses-1")
        client.write_memory("ses-1", MemoryAddress.segmented(0x812, 0x200), b"\xDE\xAD", request_id="write-once")
        client.write_memory("ses-1", MemoryAddress.segmented(0x812, 0x200), b"\xDE\xAD", request_id="write-once")
        write_requests = [request for request in transport.requests if request["method"] == "memory.write"]
        self.assertEqual(["write-once", "write-once"], [request["id"] for request in write_requests])
        self.assertEqual(base64.b64encode(b"\xDE\xAD").decode("ascii"), write_requests[0]["params"]["data_base64"])


if __name__ == "__main__":
    unittest.main()
