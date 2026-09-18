#!/usr/bin/env python3
"""Require the debugger server, typed client, and agent-facing method index to agree."""

from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "src/agent/server/agent_server.cpp"
CLIENT = ROOT / "client/python/dosbox_agent/client.py"
DOCUMENT = ROOT / "docs/DEBUGGER_AGENT_API.md"


def _between(text: str, start: str, end: str, source: Path) -> str:
    try:
        return text.split(start, 1)[1].split(end, 1)[0]
    except IndexError as error:
        raise SystemExit(f"{source}: missing {start!r} or {end!r}") from error


def _server_methods(text: str) -> list[str]:
    body = _between(text, "static bool IsKnownMethod", "\n}\n", SERVER)
    return re.findall(r'method == "([a-z][a-z0-9_.]+)"', body)


def _client_methods(text: str) -> list[str]:
    return re.findall(r'self\.call\("([a-z][a-z0-9_.]+)"', text)


def _documented_methods(text: str) -> list[str]:
    body = _between(
        text,
        "<!-- BEGIN RPC METHOD INVENTORY -->",
        "<!-- END RPC METHOD INVENTORY -->",
        DOCUMENT,
    )
    return re.findall(r'^\| `([a-z][a-z0-9_.]+)` \|', body, re.MULTILINE)


def _detailed_methods(text: str) -> set[str]:
    body = _between(
        text,
        "<!-- BEGIN RPC METHOD DETAILS -->",
        "<!-- END RPC METHOD DETAILS -->",
        DOCUMENT,
    )
    return set(re.findall(r'`([a-z][a-z0-9_.]+)`', body))


def _example_methods(text: str) -> list[str]:
    body = _between(
        text,
        "<!-- BEGIN RPC METHOD EXAMPLES -->",
        "<!-- END RPC METHOD EXAMPLES -->",
        DOCUMENT,
    )
    methods: list[str] = []
    for line_number, line in enumerate(body.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("```"):
            continue
        try:
            request = json.loads(line)
        except json.JSONDecodeError as error:
            raise SystemExit(
                f"{DOCUMENT}: invalid JSON-RPC example line {line_number}: {error}"
            ) from error
        if not isinstance(request, dict) or request.get("jsonrpc") != "2.0" or \
                not isinstance(request.get("id"), str) or \
                not isinstance(request.get("method"), str) or \
                not isinstance(request.get("params"), dict):
            raise SystemExit(
                f"{DOCUMENT}: malformed JSON-RPC example line {line_number}"
            )
        methods.append(request["method"])
    return methods


def _duplicates(values: list[str]) -> list[str]:
    return sorted({value for value in values if values.count(value) > 1})


def main() -> int:
    groups = {
        "server": _server_methods(SERVER.read_text(encoding="utf-8")),
        "client": _client_methods(CLIENT.read_text(encoding="utf-8")),
        "document": _documented_methods(DOCUMENT.read_text(encoding="utf-8")),
        "examples": _example_methods(DOCUMENT.read_text(encoding="utf-8")),
    }
    failed = False
    # The client may intentionally offer several convenience wrappers for one raw
    # method (for example the four breakpoint constructors). The server and the
    # canonical documentation inventory must each name a method exactly once.
    for name in ("server", "document", "examples"):
        values = groups[name]
        duplicates = _duplicates(values)
        if duplicates:
            failed = True
            print(f"FAIL {name}: duplicate methods: {', '.join(duplicates)}")

    expected = set(groups["server"])
    for name in ("client", "document", "examples"):
        actual = set(groups[name])
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        if missing:
            failed = True
            print(f"FAIL {name}: missing server methods: {', '.join(missing)}")
        if extra:
            failed = True
            print(f"FAIL {name}: methods absent from server: {', '.join(extra)}")

    missing_details = sorted(expected - _detailed_methods(
        DOCUMENT.read_text(encoding="utf-8")
    ))
    if missing_details:
        failed = True
        print("FAIL document: methods missing from detailed reference: " +
              ", ".join(missing_details))

    if failed:
        return 1
    print(
        f"PASS debugger agent API: {len(expected)} server methods have "
        "inventory, detail, raw-example, and typed-client entries"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
