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


def _boolean(value: Any, name: str) -> bool:
    if not isinstance(value, bool):
        raise ValueError(f"{name} must be a boolean")
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
class InterruptBreakpoint:
    number: int
    ah: int | None = None
    al: int | None = None

    def to_rpc(self) -> dict[str, str]:
        result = {
            "type": "software_interrupt",
            "number": format_hex(self.number, 2),
        }
        if self.ah is not None:
            result["ah"] = format_hex(self.ah, 2)
        if self.al is not None:
            result["al"] = format_hex(self.al, 2)
        return result

    @classmethod
    def from_rpc(cls, value: Mapping[str, Any]) -> "InterruptBreakpoint":
        if _string(value.get("type"), "breakpoint.event.type") != "software_interrupt":
            raise ValueError("breakpoint.event.type must be software_interrupt")
        return cls(
            number=int(_hex_argument(value.get("number"), 2), 16),
            ah=int(_hex_argument(value.get("ah"), 2), 16)
            if value.get("ah") is not None else None,
            al=int(_hex_argument(value.get("al"), 2), 16)
            if value.get("al") is not None else None,
        )


@dataclass(frozen=True)
class InterruptEvent:
    number: int
    ah: int
    al: int
    phase: str

    @classmethod
    def from_rpc(cls, value: Mapping[str, Any]) -> "InterruptEvent":
        if _string(value.get("type"), "stop_reason.event.type") != "software_interrupt":
            raise ValueError("stop_reason.event.type must be software_interrupt")
        phase = _string(value.get("phase"), "stop_reason.event.phase")
        if phase != "before_handler":
            raise ValueError("stop_reason.event.phase must be before_handler")
        return cls(
            number=int(_hex_argument(value.get("number"), 2), 16),
            ah=int(_hex_argument(value.get("ah"), 2), 16),
            al=int(_hex_argument(value.get("al"), 2), 16),
            phase=phase,
        )


@dataclass(frozen=True)
class StopReason:
    kind: str
    message: str | None = None
    address: MemoryAddress | None = None
    breakpoint_id: str | None = None
    psp: int | None = None
    exit_code: int | None = None
    tsr: bool | None = None
    access: WatchpointAccess | None = None
    registers: RegisterSnapshot | None = None
    hit_count: int | None = None
    event: InterruptEvent | None = None

    @classmethod
    def from_rpc(cls, value: Mapping[str, Any]) -> "StopReason":
        address = value.get("address")
        psp = value.get("psp")
        exit_code = value.get("exit_code")
        tsr = value.get("tsr")
        access = value.get("access")
        registers = value.get("registers")
        hit_count = value.get("hit_count")
        event = value.get("event")
        message = value.get("message")
        return cls(
            kind=_string(value.get("kind"), "stop_reason.kind"),
            message=_string(message, "stop_reason.message") if message is not None else None,
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
            event=InterruptEvent.from_rpc(
                _mapping(event, "stop_reason.event")
            ) if event is not None else None,
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
class RunUntilOperation:
    id: str
    session_id: str
    state_revision: int
    predicate_id: str


@dataclass(frozen=True)
class Checkpoint:
    id: str
    label: str
    captured_revision: int
    byte_count: int
    sha256: str


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
class KeyboardEvent:
    key: str
    pressed: bool

    def to_rpc(self) -> dict[str, Any]:
        if not isinstance(self.key, str) or not self.key:
            raise ValueError("keyboard key must be a non-empty string")
        if not isinstance(self.pressed, bool):
            raise ValueError("keyboard pressed must be a boolean")
        return {"key": self.key, "pressed": self.pressed}


@dataclass(frozen=True)
class JoystickState:
    index: int
    enabled: bool
    x: int
    y: int
    buttons: tuple[bool, bool]

    @classmethod
    def from_rpc(cls, value: Mapping[str, Any]) -> "JoystickState":
        axes = _mapping(value.get("axes"), "joystick.axes")
        buttons = value.get("buttons")
        if not isinstance(buttons, list) or len(buttons) != 2:
            raise ValueError("joystick.buttons must contain exactly two booleans")
        return cls(
            index=_integer(value.get("index"), "joystick.index"),
            enabled=_boolean(value.get("enabled"), "joystick.enabled"),
            x=_integer(axes.get("x"), "joystick.axes.x"),
            y=_integer(axes.get("y"), "joystick.axes.y"),
            buttons=(_boolean(buttons[0], "joystick.buttons[0]"),
                     _boolean(buttons[1], "joystick.buttons[1]")),
        )


@dataclass(frozen=True)
class InputState:
    pressed_keys: tuple[str, ...]
    joysticks: tuple[JoystickState, JoystickState]
    state_revision: int

    @classmethod
    def from_rpc(cls, value: Mapping[str, Any]) -> "InputState":
        keyboard = _mapping(value.get("keyboard"), "input.keyboard")
        pressed = keyboard.get("pressed")
        joysticks = value.get("joysticks")
        if not isinstance(pressed, list) or not all(isinstance(key, str) for key in pressed):
            raise ValueError("input.keyboard.pressed must contain only strings")
        if not isinstance(joysticks, list) or len(joysticks) != 2:
            raise ValueError("input.joysticks must contain exactly two states")
        decoded = tuple(JoystickState.from_rpc(_mapping(item, "joystick"))
                        for item in joysticks)
        if tuple(item.index for item in decoded) != (0, 1):
            raise ValueError("input.joysticks must be ordered by index 0, 1")
        return cls(
            pressed_keys=tuple(pressed),
            joysticks=(decoded[0], decoded[1]),
            state_revision=_integer(value.get("state_revision"), "input.state_revision"),
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
class RunUntilPredicate:
    kind: str
    address: MemoryAddress | None = None
    length: int = 1
    condition: BreakpointCondition | None = None
    hit_filter: BreakpointHitFilter = BreakpointHitFilter()
    event: InterruptBreakpoint | None = None

    def to_rpc(self) -> dict[str, Any]:
        supported = ("execution", "interrupt", "memory_change", "memory_read",
                     "memory_write", "memory_access")
        if self.kind not in supported:
            raise ValueError("unsupported run-until predicate kind")
        if (not isinstance(self.length, int) or isinstance(self.length, bool) or
                self.length <= 0):
            raise ValueError("run-until predicate length must be positive")
        if self.condition is not None and self.kind not in ("execution", "interrupt"):
            raise ValueError("register conditions require an execution or interrupt predicate")
        result: dict[str, Any] = {
            "kind": self.kind,
            "hit_filter": self.hit_filter.to_rpc(),
        }
        if self.condition is not None:
            result["condition"] = self.condition.to_rpc()
        if self.kind == "interrupt":
            if self.event is None or self.address is not None:
                raise ValueError("interrupt predicates require event and no address")
            result["event"] = self.event.to_rpc()
            return result
        if self.address is None or self.event is not None:
            raise ValueError("non-interrupt predicates require address and no event")
        if self.kind in ("execution", "memory_change") and self.length != 1:
            raise ValueError("execution and memory_change predicates require length 1")
        result["address"] = self.address.to_rpc()
        result["length"] = self.length
        return result


@dataclass(frozen=True)
class Breakpoint:
    id: str
    kind: str
    address: MemoryAddress | None
    once: bool
    length: int = 1
    enabled: bool = True
    condition: BreakpointCondition | None = None
    hit_filter: BreakpointHitFilter = BreakpointHitFilter()
    event: InterruptBreakpoint | None = None


@dataclass(frozen=True)
class DosProgramLoad:
    name: str
    format: str
    psp: str
    load_segment: str
    image_bytes: int
    entry_segment: str
    entry_offset: str
    initial_stack_segment: str
    initial_stack_offset: str

    @classmethod
    def from_rpc(cls, value: Mapping[str, Any]) -> "DosProgramLoad":
        entry = _mapping(value.get("entry"), "dos.memory_map.target.entry")
        stack = _mapping(value.get("initial_stack"), "dos.memory_map.target.initial_stack")
        return cls(
            name=_string(value.get("name"), "dos.memory_map.target.name"),
            format=_string(value.get("format"), "dos.memory_map.target.format"),
            psp=_string(value.get("psp"), "dos.memory_map.target.psp"),
            load_segment=_string(value.get("load_segment"), "dos.memory_map.target.load_segment"),
            image_bytes=_integer(value.get("image_bytes"), "dos.memory_map.target.image_bytes"),
            entry_segment=_string(entry.get("segment"), "dos.memory_map.target.entry.segment"),
            entry_offset=_string(entry.get("offset"), "dos.memory_map.target.entry.offset"),
            initial_stack_segment=_string(stack.get("segment"), "dos.memory_map.target.initial_stack.segment"),
            initial_stack_offset=_string(stack.get("offset"), "dos.memory_map.target.initial_stack.offset"),
        )


@dataclass(frozen=True)
class DosMemoryBlock:
    mcb_segment: str
    data_segment: str
    paragraphs: int
    bytes: int
    owner_psp: str
    name: str
    last: bool
    process: bool
    target_owned: bool
    parent_psp: str | None = None
    environment_segment: str | None = None

    @classmethod
    def from_rpc(cls, value: Mapping[str, Any]) -> "DosMemoryBlock":
        parent = value.get("parent_psp")
        environment = value.get("environment_segment")
        return cls(
            mcb_segment=_string(value.get("mcb_segment"), "dos.memory_map.block.mcb_segment"),
            data_segment=_string(value.get("data_segment"), "dos.memory_map.block.data_segment"),
            paragraphs=_integer(value.get("paragraphs"), "dos.memory_map.block.paragraphs"),
            bytes=_integer(value.get("bytes"), "dos.memory_map.block.bytes"),
            owner_psp=_string(value.get("owner_psp"), "dos.memory_map.block.owner_psp"),
            name=_string(value.get("name"), "dos.memory_map.block.name"),
            last=_boolean(value.get("last"), "dos.memory_map.block.last"),
            process=_boolean(value.get("process"), "dos.memory_map.block.process"),
            target_owned=_boolean(value.get("target_owned"), "dos.memory_map.block.target_owned"),
            parent_psp=_string(parent, "dos.memory_map.block.parent_psp") if parent is not None else None,
            environment_segment=_string(environment, "dos.memory_map.block.environment_segment")
            if environment is not None else None,
        )


@dataclass(frozen=True)
class DosMemoryMap:
    current_psp: str
    first_mcb: str
    target: DosProgramLoad
    blocks: tuple[DosMemoryBlock, ...]


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
class TraceEffect:
    kind: str
    byte_count: int
    address: MemoryAddress | None = None
    port: str | None = None
    data: bytes | None = None
    before: bytes | None = None
    after: bytes | None = None
    value: str | None = None

    @classmethod
    def from_rpc(cls, value: Mapping[str, Any]) -> "TraceEffect":
        kind = _string(value.get("kind"), "trace.effect.kind")
        byte_count = _integer(value.get("byte_count"), "trace.effect.byte_count")
        if byte_count not in (1, 2, 4):
            raise ValueError("trace.effect.byte_count must be 1, 2, or 4")
        if kind == "memory_read":
            try:
                data = base64.b64decode(
                    _string(value.get("data_base64"), "trace.effect.data_base64"), validate=True
                )
            except (binascii.Error, ValueError) as error:
                raise ValueError("trace.effect.data_base64 is invalid") from error
            if len(data) != byte_count:
                raise ValueError("trace.effect.data_base64 length does not match byte_count")
            return cls(kind, byte_count,
                       address=MemoryAddress.from_rpc(_mapping(value.get("address"), "trace.effect.address")),
                       data=data)
        if kind == "memory_write":
            try:
                before = base64.b64decode(
                    _string(value.get("before_base64"), "trace.effect.before_base64"), validate=True
                )
                after = base64.b64decode(
                    _string(value.get("after_base64"), "trace.effect.after_base64"), validate=True
                )
            except (binascii.Error, ValueError) as error:
                raise ValueError("trace.effect write bytes are invalid") from error
            if len(before) != byte_count or len(after) != byte_count:
                raise ValueError("trace.effect write byte length does not match byte_count")
            return cls(kind, byte_count,
                       address=MemoryAddress.from_rpc(_mapping(value.get("address"), "trace.effect.address")),
                       before=before, after=after)
        if kind in ("io_read", "io_write"):
            return cls(kind, byte_count,
                       port=_string(value.get("port"), "trace.effect.port"),
                       value=_string(value.get("value"), "trace.effect.value"))
        raise ValueError(f"unsupported trace effect kind: {kind}")


@dataclass(frozen=True)
class TraceEvent:
    sequence: int
    address: MemoryAddress
    instruction: str
    register_changes: Mapping[str, str]
    effects: tuple[TraceEffect, ...]


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
