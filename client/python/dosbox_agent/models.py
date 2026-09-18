from __future__ import annotations

import base64
import binascii
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping


def _string(value: Any, name: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{name} must be a string")
    return value


def _integer(value: Any, name: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise ValueError(f"{name} must be an integer")
    return value


def _mapping(value: Any, name: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise ValueError(f"{name} must be an object")
    return value


def format_hex(value: int, width: int) -> str:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0 or value >= 1 << (width * 4):
        raise ValueError(f"value does not fit in an unsigned {width * 4}-bit hexadecimal field")
    return f"0x{value:0{width}X}"


@dataclass(frozen=True)
class AgentConfig:
    path: Path
    transport: str
    endpoint: str
    dosbox_executable: Path
    dosbox_workdir: Path
    request_timeout_ms: int
    max_message_bytes: int
    max_memory_read_bytes: int
    max_trace_events: int
    test_profile: bool = False


@dataclass(frozen=True)
class MemoryAddress:
    space: str
    offset: str
    segment: str | None = None

    @classmethod
    def segmented(cls, segment: str | int, offset: str | int) -> "MemoryAddress":
        return cls("segmented", _hex_argument(offset, 8), _hex_argument(segment, 4))

    @classmethod
    def linear(cls, offset: str | int) -> "MemoryAddress":
        return cls("linear", _hex_argument(offset, 8))

    @classmethod
    def physical(cls, offset: str | int) -> "MemoryAddress":
        return cls("physical", _hex_argument(offset, 8))

    def to_rpc(self) -> dict[str, str]:
        result = {"space": self.space, "offset": self.offset}
        if self.segment is not None:
            result["segment"] = self.segment
        return result

    @classmethod
    def from_rpc(cls, value: Mapping[str, Any]) -> "MemoryAddress":
        space = _string(value.get("space"), "address.space")
        offset = _string(value.get("offset"), "address.offset")
        segment = value.get("segment")
        if segment is not None:
            segment = _string(segment, "address.segment")
        return cls(space=space, offset=offset, segment=segment)


def _hex_argument(value: str | int, width: int) -> str:
    if isinstance(value, int) and not isinstance(value, bool):
        return format_hex(value, width)
    if isinstance(value, str) and value.startswith("0x") and len(value) == width + 2:
        int(value[2:], 16)
        return "0x" + value[2:].upper()
    raise ValueError(f"hexadecimal argument must be an integer or 0x-prefixed {width}-digit string")


@dataclass(frozen=True)
class WatchpointAccess:
    kind: str
    address: MemoryAddress
    instruction_address: MemoryAddress
    before: bytes
    after: bytes

    @classmethod
    def from_rpc(cls, value: Mapping[str, Any]) -> "WatchpointAccess":
        kind = _string(value.get("kind"), "stop_reason.access.kind")
        if kind not in ("read", "write"):
            raise ValueError("stop_reason.access.kind must be read or write")
        try:
            before = base64.b64decode(
                _string(value.get("before_base64"), "stop_reason.access.before_base64"),
                validate=True,
            )
            after = base64.b64decode(
                _string(value.get("after_base64"), "stop_reason.access.after_base64"),
                validate=True,
            )
        except (ValueError, binascii.Error) as error:
            raise ValueError("stop_reason.access contains invalid base64") from error
        byte_count = _integer(value.get("byte_count"), "stop_reason.access.byte_count")
        if len(before) != byte_count or len(after) != byte_count:
            raise ValueError("stop_reason.access byte_count does not match its values")
        return cls(
            kind=kind,
            address=MemoryAddress.from_rpc(
                _mapping(value.get("address"), "stop_reason.access.address")
            ),
            instruction_address=MemoryAddress.from_rpc(
                _mapping(value.get("instruction_address"),
                         "stop_reason.access.instruction_address")
            ),
            before=before,
            after=after,
        )


@dataclass(frozen=True)
class StopReason:
    kind: str
    address: MemoryAddress | None = None
    breakpoint_id: str | None = None
    psp: int | None = None
    exit_code: int | None = None
    tsr: bool | None = None
    access: WatchpointAccess | None = None
    registers: RegisterSnapshot | None = None
    hit_count: int | None = None

    @classmethod
    def from_rpc(cls, value: Mapping[str, Any]) -> "StopReason":
        address = value.get("address")
        psp = value.get("psp")
        exit_code = value.get("exit_code")
        tsr = value.get("tsr")
        access = value.get("access")
        registers = value.get("registers")
        hit_count = value.get("hit_count")
        return cls(
            kind=_string(value.get("kind"), "stop_reason.kind"),
            address=MemoryAddress.from_rpc(_mapping(address, "stop_reason.address")) if address is not None else None,
            breakpoint_id=value.get("breakpoint_id") if isinstance(value.get("breakpoint_id"), str) else None,
            psp=_integer(psp, "stop_reason.psp") if psp is not None else None,
            exit_code=_integer(exit_code, "stop_reason.exit_code") if exit_code is not None else None,
            tsr=tsr if isinstance(tsr, bool) else None,
            access=WatchpointAccess.from_rpc(
                _mapping(access, "stop_reason.access")
            ) if access is not None else None,
            registers=RegisterSnapshot.from_rpc(
                _mapping(registers, "stop_reason.registers")
            ) if registers is not None else None,
            hit_count=_integer(hit_count, "stop_reason.hit_count")
            if hit_count is not None else None,
        )


@dataclass(frozen=True)
class Session:
    id: str
    state: str
    state_revision: int
    stop_reason: StopReason | None = None
    target_psp: int | None = None


@dataclass(frozen=True)
class Operation:
    id: str
    session_id: str
    state_revision: int


@dataclass(frozen=True)
class WaitResult:
    session: Session
    running: bool


@dataclass(frozen=True)
class RegisterSnapshot:
    general: Mapping[str, str]
    segments: Mapping[str, str]
    instruction_pointer: str
    flags: str
    cpu_mode: str
    state_revision: int
    phase: str | None = None

    @classmethod
    def from_rpc(cls, value: Mapping[str, Any]) -> "RegisterSnapshot":
        phase = value.get("phase")
        return cls(
            general=dict(_mapping(value.get("general"), "registers.general")),
            segments=dict(_mapping(value.get("segments"), "registers.segments")),
            instruction_pointer=_string(
                value.get("instruction_pointer"), "registers.instruction_pointer"
            ),
            flags=_string(value.get("flags"), "registers.flags"),
            cpu_mode=_string(value.get("cpu_mode"), "registers.cpu_mode"),
            state_revision=_integer(value.get("state_revision", 0),
                                    "registers.state_revision"),
            phase=_string(phase, "registers.phase") if phase is not None else None,
        )


@dataclass(frozen=True)
class MemoryRead:
    address: MemoryAddress
    data: bytes
    sha256: str
    state_revision: int


@dataclass(frozen=True)
class MemoryWrite:
    address: MemoryAddress
    byte_count: int
    before_sha256: str
    after_sha256: str
    state_revision: int


@dataclass(frozen=True)
class SnapshotBlock:
    data: bytes
    sha256: str


@dataclass(frozen=True)
class VideoFrame:
    block: SnapshotBlock
    kind: str
    width: int
    height: int
    bpp: int
    pitch: int
    double_width: bool
    double_height: bool


@dataclass(frozen=True)
class VideoSnapshot:
    state_revision: int
    captured_ticks: int
    video_mode: int
    text: SnapshotBlock
    text_columns: int
    glyph_height: int
    text_start_offset: int
    fonts: SnapshotBlock
    font_page_count: int
    font_glyph_count: int
    font_glyph_stride: int
    dac: SnapshotBlock
    dac_bits: int
    dac_pel_mask: int
    renderer_palette: SnapshotBlock
    frame: VideoFrame
    geometry: Mapping[str, Any]
    crtc: SnapshotBlock


@dataclass(frozen=True)
class BreakpointCondition:
    register: str
    operator: str
    value: int

    def to_rpc(self) -> dict[str, Any]:
        if self.operator not in ("eq", "ne"):
            raise ValueError("breakpoint condition operator must be eq or ne")
        if not isinstance(self.value, int) or isinstance(self.value, bool) or not 0 <= self.value <= 0xffffffff:
            raise ValueError("breakpoint condition value must be a 32-bit unsigned integer")
        return {"register": self.register, "operator": self.operator,
                "value": f"0x{self.value:08X}"}

    @classmethod
    def from_rpc(cls, value: Mapping[str, Any]) -> "BreakpointCondition":
        encoded_value = _string(value.get("value"), "breakpoint.condition.value")
        if len(encoded_value) != 10 or not encoded_value.startswith("0x"):
            raise ValueError("breakpoint.condition.value must be 0xNNNNNNNN")
        try:
            parsed_value = int(encoded_value[2:], 16)
        except ValueError as error:
            raise ValueError("breakpoint.condition.value must be hexadecimal") from error
        return cls(
            register=_string(value.get("register"), "breakpoint.condition.register"),
            operator=_string(value.get("operator"), "breakpoint.condition.operator"),
            value=parsed_value,
        )


@dataclass(frozen=True)
class BreakpointHitFilter:
    skip: int = 0
    every: int = 1

    def to_rpc(self) -> dict[str, int]:
        if (not isinstance(self.skip, int) or isinstance(self.skip, bool) or self.skip < 0 or
                not isinstance(self.every, int) or isinstance(self.every, bool) or self.every <= 0):
            raise ValueError("hit filter requires non-negative skip and positive every")
        return {"skip": self.skip, "every": self.every}

    @classmethod
    def from_rpc(cls, value: Mapping[str, Any]) -> "BreakpointHitFilter":
        return cls(
            skip=_integer(value.get("skip"), "breakpoint.hit_filter.skip"),
            every=_integer(value.get("every"), "breakpoint.hit_filter.every"),
        )


@dataclass(frozen=True)
class Breakpoint:
    id: str
    kind: str
    address: MemoryAddress
    once: bool
    length: int = 1
    enabled: bool = True
    condition: BreakpointCondition | None = None
    hit_filter: BreakpointHitFilter = BreakpointHitFilter()


@dataclass(frozen=True)
class OutputRecord:
    sequence: int
    timestamp: int
    level: str
    source: str
    message: str


@dataclass(frozen=True)
class OutputPage:
    records: tuple[OutputRecord, ...]
    next_cursor: str | None


@dataclass(frozen=True)
class TraceEvent:
    sequence: int
    address: MemoryAddress
    instruction: str
    register_changes: Mapping[str, str]


@dataclass(frozen=True)
class TracePage:
    active: bool
    events: tuple[TraceEvent, ...]
    next_cursor: str | None


@dataclass(frozen=True)
class DiagnosticCommandResult:
    accepted: bool
    raw_output: str
    parse_status: str
    parsed_registers: Mapping[str, str] | None
    unparsed_output: str | None
