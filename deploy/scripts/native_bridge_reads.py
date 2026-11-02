#!/usr/bin/env python3
"""What the C++ Native Bridge registers, reads and publishes.

generate_native_contract.py compares the authored Native API with the code
that serves it, and this module reads that code. It reads C++ as text, so it
knows the forms the Bridge is written in, and reports what it cannot follow
instead of guessing:

- A method is registered as
  register_handler("name", ExecutionPolicy::X, callback). The callback is a
  function's name (ns::fn), a lambda, or a wrapper around a function's name,
  such as with_viewer(ns::fn). A commented-out registration is not one.
- A request parameter is read as params.at("key"), params["key"],
  params.value("key", ...), params.contains("key"), params.find("key"),
  params.count("key"), or through a helper called as helper(params, "key").
  A helper named required_* or require_* requires the key; one named
  optional_* reads it as optional.
- params handed whole to another function is followed into that function's
  definition: in the Bridge, or in a domain file that takes the raw params.
- declared_params(params, {"a", "b"}) hands on only the keys it names.
- A read whose key is not a literal (a local lambda or a loop over names)
  makes the method "dynamic": a declared key that appears as a literal in the
  code read counts as read.

Each key read is recorded with how it is read: "required" (at, [], a
required_* helper), "optional" (value, an optional_* helper), "guarded"
(contains, find, count) or "unknown" (any other helper).
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Registration:
    """One register_handler call and what its handler reads."""

    name: str
    policy: str
    file: str
    keys: dict[str, set[str]] = field(default_factory=dict)
    dynamic: bool = False
    literals: set[str] = field(default_factory=set)
    unresolved: list[str] = field(default_factory=list)


@dataclass
class Definition:
    namespace: str
    parameters: list[str | None]  # the name of each Json parameter ("" unnamed), None for others
    body: str
    file: str


KEYWORDS = {
    "if", "for", "while", "switch", "catch", "return", "sizeof", "defined", "decltype",
    "static_assert", "alignof", "noexcept", "throw", "new", "delete",
}
JSON_PARAMETER = re.compile(r"^const\s+(?:Json|nlohmann::json|nlohmann::ordered_json)\s*&\s*(\w*)$")
DEFINITION = re.compile(
    r"(?<![\w:.>~])([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->\s*[\w:<>, *&]+?\s*)?\{")
NAMESPACE = re.compile(r"\bnamespace\s+([A-Za-z_][\w:]*)\s*\{")
CALL = re.compile(r"(?<![\w.>])([A-Za-z_][\w:]*)\s*\(")
REGISTRATION = re.compile(r'\bregister_handler\(\s*"([A-Za-z][A-Za-z0-9.]*)"')
STRING_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
# Calls that take params whole and read none of its keys.
NON_READERS = re.compile(r"^(?:std::\w[\w:]*|Json|nlohmann::\w[\w:]*|static_cast|const_cast|reinterpret_cast)$")
FILTER = "declared_params"


def _skip_literal(text: str, index: int) -> int:
    """Index just past the string or character literal starting at text[index]."""
    quote = text[index]
    if quote == '"' and index > 0 and text[index - 1] == "R":
        match = re.match(r'"([^()\\ \n]{0,16})\(', text[index:])
        if match:
            end = text.find(")" + match.group(1) + '"', index)
            return len(text) if end < 0 else end + len(match.group(1)) + 2
    position = index + 1
    while position < len(text) and text[position] != quote:
        position += 2 if text[position] == "\\" else 1
    return position + 1


def without_comments(text: str) -> str:
    """The text with each comment blanked, its line breaks kept."""
    out: list[str] = []
    index = 0
    while index < len(text):
        if text.startswith("//", index):
            end = text.find("\n", index)
            end = len(text) if end < 0 else end
            out.append(" " * (end - index))
            index = end
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = len(text) if end < 0 else end + 2
            out.append(re.sub(r"[^\n]", " ", text[index:end]))
            index = end
        elif text[index] in "\"'" and not (text[index] == "'" and index > 0 and text[index - 1].isalnum()):
            end = _skip_literal(text, index)
            out.append(text[index:end])
            index = end
        else:
            out.append(text[index])
            index += 1
    return "".join(out)


def matching(text: str, start: int, opening: str, closing: str) -> int:
    """Index just past the bracket that closes the one at text[start]."""
    depth = 0
    index = start
    while index < len(text):
        character = text[index]
        if character in "\"'" and not (character == "'" and index > 0 and text[index - 1].isalnum()):
            index = _skip_literal(text, index)
            continue
        if character == opening:
            depth += 1
        elif character == closing:
            depth -= 1
            if depth == 0:
                return index + 1
        index += 1
    raise ValueError("unbalanced brackets")


def split_arguments(text: str) -> list[str]:
    """Top-level comma-separated arguments of a call's inside."""
    parts: list[str] = []
    depth = 0
    current: list[str] = []
    index = 0
    while index < len(text):
        character = text[index]
        if character in "\"'" and not (character == "'" and index > 0 and text[index - 1].isalnum()):
            end = _skip_literal(text, index)
            current.append(text[index:end])
            index = end
            continue
        if character in "([{":
            depth += 1
        elif character in ")]}":
            depth -= 1
        if character == "," and depth == 0:
            parts.append("".join(current).strip())
            current = []
        else:
            current.append(character)
        index += 1
    if "".join(current).strip():
        parts.append("".join(current).strip())
    return parts


def json_parameters(arguments: str) -> list[str | None]:
    result: list[str | None] = []
    for argument in split_arguments(arguments):
        declaration = argument.split("=")[0].strip()
        match = JSON_PARAMETER.match(declaration)
        result.append(match.group(1) if match else None)
    return result


class Index:
    """Every function definition that takes a Json parameter, by its name."""

    def __init__(self, files: list[Path], root: Path):
        self.definitions: dict[str, list[Definition]] = {}
        self.sources: dict[str, str] = {}
        for path in files:
            text = without_comments(path.read_text(encoding="utf-8", errors="replace"))
            relative = path.relative_to(root).as_posix()
            self.sources[relative] = text
            spans = self._namespaces(text)
            for match in DEFINITION.finditer(text):
                name, arguments = match.group(1), match.group(2)
                if name in KEYWORDS or ("json" not in arguments.lower()):
                    continue
                parameters = json_parameters(arguments)
                if all(parameter is None for parameter in parameters):
                    continue
                start = match.end() - 1
                try:
                    end = matching(text, start, "{", "}")
                except ValueError:
                    continue
                self.definitions.setdefault(name, []).append(
                    Definition(self._namespace_at(spans, match.start()), parameters, text[start:end], relative))

    @staticmethod
    def _namespaces(text: str) -> list[tuple[int, int, str]]:
        spans = []
        for match in NAMESPACE.finditer(text):
            start = match.end() - 1
            try:
                end = matching(text, start, "{", "}")
            except ValueError:
                end = len(text)
            spans.append((start, end, match.group(1).split("::")[-1]))
        return spans

    @staticmethod
    def _namespace_at(spans: list[tuple[int, int, str]], offset: int) -> str:
        best, best_start = "", -1
        for start, end, name in spans:
            if start <= offset < end and start > best_start:
                best, best_start = name, start
        return best

    def resolve(self, callee: str, namespace: str) -> list[Definition]:
        short = callee.split("::")[-1]
        candidates = self.definitions.get(short, [])
        if "::" in callee:
            qualifier = callee.split("::")[-2]
            return [candidate for candidate in candidates if candidate.namespace == qualifier]
        same = [candidate for candidate in candidates if candidate.namespace == namespace]
        return same or candidates


def _accessor_kind(helper: str) -> str:
    short = helper.split("::")[-1]
    if short.startswith(("required", "require")):
        return "required"
    if short.startswith("optional"):
        return "optional"
    return "unknown"


class Reader:
    def __init__(self, index: Index):
        self.index = index

    def read(self, registration: Registration, namespace: str, variable: str, body: str,
             seen: set[tuple[str, str, int]] | None = None, keep: set[str] | None = None) -> None:
        """Records what body reads from the Json named variable.

        keep, when given, limits the keys recorded to those a declared_params
        filter handed on."""
        seen = set() if seen is None else seen
        key = (namespace, variable, hash(body))
        if key in seen:
            return
        seen.add(key)
        name = re.escape(variable)

        def record(found: str, kind: str) -> None:
            if keep is None or found in keep:
                registration.keys.setdefault(found, set()).add(kind)

        member = re.compile(r"\b" + name + r"\s*\.\s*(at|value|contains|find|count)\s*\(\s*(\"[^\"]*\"|[A-Za-z_][\w:]*)")
        for match in member.finditer(body):
            operation, argument = match.group(1), match.group(2)
            if not argument.startswith('"'):
                registration.dynamic = True
                continue
            kind = {"at": "required", "value": "optional"}.get(operation, "guarded")
            record(argument[1:-1], kind)
        for match in re.finditer(r"\b" + name + r"\s*\[\s*(\"[^\"]*\"|[A-Za-z_][\w:]*)\s*\]", body):
            if match.group(1).startswith('"'):
                record(match.group(1)[1:-1], "required")
            else:
                registration.dynamic = True
        if re.search(r"\b" + name + r"\s*\.\s*items\s*\(", body) or re.search(r":\s*" + name + r"\s*\)", body):
            registration.dynamic = True
        for literal in STRING_LITERAL.findall(body):
            if keep is None or literal in keep:
                registration.literals.add(literal)

        for match in CALL.finditer(body):
            callee = match.group(1)
            if callee.split("::")[-1] in KEYWORDS:
                continue
            opening = match.end() - 1
            try:
                closing = matching(body, opening, "(", ")")
            except ValueError:
                continue
            arguments = split_arguments(body[opening + 1:closing - 1])
            # helper(params, "key", ...): a read through a named accessor.
            if len(arguments) >= 2 and arguments[0] == variable and re.fullmatch(r'"[^"]*"', arguments[1]):
                record(arguments[1][1:-1], _accessor_kind(callee))
                continue
            if callee.split("::")[-1] == FILTER:
                continue  # followed below, as the argument of the call it is handed to
            for position, argument in enumerate(arguments):
                filtered = re.fullmatch(FILTER + r"\s*\(\s*" + name + r"\s*,\s*\{(.*)\}\s*\)", argument, re.S)
                if argument != variable and not filtered:
                    continue
                kept = set(re.findall(r'"([^"]*)"', filtered.group(1))) if filtered else None
                if kept is not None and keep is not None:
                    kept &= keep
                elif kept is None:
                    kept = keep
                self._follow(registration, callee, namespace, position, seen, kept)

    def _follow(self, registration: Registration, callee: str, namespace: str, position: int,
                seen: set[tuple[str, str, int]], keep: set[str] | None) -> None:
        if NON_READERS.match(callee):
            return
        definitions = [
            definition for definition in self.index.resolve(callee, namespace)
            if position < len(definition.parameters) and definition.parameters[position] is not None
        ]
        if not definitions:
            if callee not in registration.unresolved:
                registration.unresolved.append(callee)
            return
        for definition in definitions:
            parameter = definition.parameters[position]
            if parameter:
                self.read(registration, definition.namespace, parameter, definition.body, seen, keep)

    def handler(self, registration: Registration, callback: str, namespace: str) -> None:
        callback = callback.strip()
        if callback.startswith("["):
            match = re.match(r"\[[^\]]*\]\s*\(([^)]*)\)", callback)
            parameters = json_parameters(match.group(1)) if match else []
            brace = callback.find("{")
            if parameters and parameters[0] and brace >= 0:
                self.read(registration, namespace, parameters[0], callback[brace:])
            return
        wrapped = re.fullmatch(r"([A-Za-z_][\w:]*)\s*\(\s*([A-Za-z_][\w:]*)\s*\)", callback)
        name = wrapped.group(2) if wrapped else callback
        definitions = [
            definition for definition in self.index.resolve(name, namespace)
            if definition.parameters and definition.parameters[0] is not None
        ]
        if not definitions:
            registration.unresolved.append(name)
            return
        for definition in definitions:
            if definition.parameters[0]:
                self.read(registration, definition.namespace, definition.parameters[0], definition.body)


def source_files(backend: Path) -> list[Path]:
    """The Bridge's sources, and every other backend source that takes raw params."""
    bridge = backend / "Bridge"
    files = [
        path for path in sorted(bridge.rglob("*"))
        if path.suffix in {".cpp", ".hpp", ".mm"} and ".test." not in path.name
    ]
    for path in sorted(backend.rglob("*")):
        if path.suffix not in {".cpp", ".hpp", ".mm"} or ".test." in path.name or bridge in path.parents:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if re.search(r"const\s+(?:Json|nlohmann::json)\s*&\s*params\b", text):
            files.append(path)
    return files


def read_bridge(backend: Path) -> dict[str, list[Registration]]:
    """Every registered method, with what its handler reads."""
    index = Index(source_files(backend), backend)
    reader = Reader(index)
    registrations: dict[str, list[Registration]] = {}
    for relative, text in index.sources.items():
        if not relative.startswith("Bridge/"):
            continue
        spans = Index._namespaces(text)
        for match in REGISTRATION.finditer(text):
            opening = text.rfind("(", 0, match.start() + len("register_handler") + 1)
            try:
                closing = matching(text, opening, "(", ")")
            except ValueError:
                continue
            arguments = split_arguments(text[opening + 1:closing - 1])
            policy = re.fullmatch(r"ExecutionPolicy::(\w+)", arguments[1]) if len(arguments) > 1 else None
            registration = Registration(match.group(1), policy.group(1) if policy else "", relative)
            if len(arguments) > 2:
                reader.handler(registration, arguments[2], Index._namespace_at(spans, match.start()))
            registrations.setdefault(registration.name, []).append(registration)
    return registrations


def embedded_client(backend: Path) -> dict[str, str]:
    """The embedded client's functions and the method each one calls.

    Read from the Native Bridge's own script (NativeBridge.cpp bridge_script()):
    `name: (...) => invoke('method', ...)`, in the script's order."""
    text = (backend / "Bridge" / "NativeBridge" / "NativeBridge.cpp").read_text(encoding="utf-8")
    client: dict[str, str] = {}
    for match in re.finditer(r"\n\s*(\w+)\s*:\s*(?:async\s*)?\([^)]*\)\s*=>\s*(?:\n\s*)?invoke\('([\w.]+)'", text):
        client.setdefault(match.group(1), match.group(2))
    return client


def published_events(backend: Path) -> set[str]:
    """Events the backend publishes, and the ones its embedded client sends itself.

    Tests and by-hand probes (`*.probe.cpp`, run outside the app) publish
    nothing: their pages are their own, not the window's."""
    events: set[str] = set()
    for path in sorted(backend.rglob("*")):
        if path.suffix not in {".cpp", ".hpp", ".mm"} or ".test." in path.name or ".probe." in path.name:
            continue
        text = without_comments(path.read_text(encoding="utf-8", errors="replace"))
        events.update(re.findall(r'\bevents::publish\(\s*"([^"]+)"', text))
        events.update(re.findall(r'\bemit\(\s*"([^"]+)"', text))
        events.update(re.findall(r"new\s+CustomEvent\(\s*'([^']+)'", text))
    return events
