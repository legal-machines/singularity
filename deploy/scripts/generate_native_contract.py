#!/usr/bin/env python3
"""Generate and verify the aggregate Native API from domain contracts."""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE_DIR = ROOT / "docs" / "contracts" / "native"
SCHEMAS = SOURCE_DIR / "Schemas.json"
GENERATED = ROOT / "docs" / "contracts" / "generated" / "NativeApi.json"
COMPATIBILITY_OUTPUT = ROOT / "docs" / "contracts" / "native-api.json"
LEGACY_BOOTSTRAP_SOURCE = COMPATIBILITY_OUTPUT
BACKEND_ROOT = ROOT / "code" / "backend"

# What the C++ Bridge registers, reads and publishes (native_bridge_reads.py).
_reads_spec = importlib.util.spec_from_file_location(
    "native_bridge_reads", Path(__file__).with_name("native_bridge_reads.py"))
assert _reads_spec is not None and _reads_spec.loader is not None
bridge_reads = importlib.util.module_from_spec(_reads_spec)
sys.modules.setdefault(_reads_spec.name, bridge_reads)
_reads_spec.loader.exec_module(bridge_reads)

DOMAINS = (
    "Platform",
    "Account",
    "Matter",
    "Chat",
    "Document",
    "Context",
    "Consilium",
    "WorkProduct",
    "Usage",
    "Routing",
    "Provider",
    "LocalModel",
    "PapinianGuard",
    "Node",
    "Privacy",
    "Agent",
    "Organisation",
    "DocumentReview",
    "Office",
    "Correspondence",
)


def domain_for(method_name: str) -> str:
    if method_name.startswith("agent."):
        return "Agent"
    if method_name.startswith(("dialog.", "permissions.", "runtime.", "system.", "window.")):
        return "Platform"
    if method_name.startswith(("onboarding.", "settings.", "notices.", "account.")):
        return "Account"
    if method_name.startswith("providers."):
        return "Provider"
    if method_name.startswith("protectedApi."):
        return "PapinianGuard"
    if method_name.startswith("privacy."):
        return "Privacy"
    if method_name.startswith("context."):
        return "Context"
    if method_name.startswith("consilium."):
        return "Consilium"
    if method_name.startswith("chat."):
        return "Chat"
    # New Document methods use their own domain's name rather than the legacy
    # matter.* wire prefix (D-001).
    if method_name.startswith("document."):
        return "Document"
    if method_name.startswith(("papinian.", "localModels.")):
        return "LocalModel"
    # Singularity's new versions from Legal Machines' site (Node/AppUpdate).
    if method_name.startswith(("workspace.", "update.")) or method_name in {
        "node.getSummary",
        "node.checkLegalMachinesConnection",
        "node.releaseReclaimableMemory",
    }:
        return "Node"
    if method_name == "routingLog.list" or method_name.startswith("usage."):
        return "Usage"
    if method_name == "models.execute" or method_name.startswith("routing."):
        return "Routing"
    if method_name in {
        "matter.createChat",
        "matter.runTask",
        "matter.previewOutboundRequest",
        "matter.revealProtectedOccurrence",
        "matter.closeOutboundPreview",
        "matter.retryTask",
        "matter.reviseTask",
        "matter.runConsilium",
        "matter.synthesizeConsilium",
        "matter.saveChatAiSetup",
    }:
        return "Chat"
    if method_name in {
        "matter.addDocument",
        "matter.removeDocument",
        "matter.revealDocument",
        "matter.openDocument",
        "matter.listDocuments",
        "matter.renameDocument",
        "matter.moveDocument",
        "matter.deleteDocument",
        "matter.createFolder",
        "matter.addExternalFile",
        "matter.reprocessDocument",
        "matter.resolveDocumentSource",
        "matter.readDocumentView",
    }:
        return "Document"
    if method_name in {
        "matter.addNote",
        "matter.removeNote",
        "matter.addIssue",
        "matter.updateIssueStatus",
        "matter.listWorkflows",
        "matter.runWorkflow",
        "matter.exportText",
    }:
        return "WorkProduct"
    if method_name.startswith(("workProduct.", "reviewTable.", "playbook.")):
        return "WorkProduct"
    if method_name.startswith("organisation."):
        return "Organisation"
    if method_name.startswith("documentReview."):
        return "DocumentReview"
    # Singularity in Word, Excel and Outlook (D-105), implemented in Office/.
    if method_name.startswith("office."):
        return "Office"
    # A Matter's correspondence (D-106), implemented in Correspondence/.
    if method_name.startswith("correspondence."):
        return "Correspondence"
    if method_name.startswith("matter."):
        return "Matter"
    # New Native methods use project.* per D-001 (Project is the only current
    # product term); matter.* stays only for the wire/compatibility names it
    # already owns. Both are authored in Matter.json and implemented in the
    # Matter domain/Bridge - project.* is not a separate contract domain.
    if method_name.startswith("project."):
        return "Matter"
    # The firm's coordination carries Matters between computers (D-098):
    # authored in Matter.json, implemented in Matter/MatterCoordination.
    if method_name.startswith("coordination."):
        return "Matter"
    raise ValueError(f"No contract domain classification for {method_name}")


def formatted(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2) + "\n"


def write_contract_file(path: Path, text: str) -> None:
    """Writes a contract file byte for byte, with LF line ends on every platform.

    Text mode on Windows writes CRLF, and the whole file would then differ."""
    path.write_bytes(text.encode("utf-8"))


def bootstrap() -> None:
    """One-time conversion of the checked-in v18 aggregate into domain sources."""
    if not LEGACY_BOOTSTRAP_SOURCE.exists():
        raise RuntimeError("The legacy aggregate is unavailable for bootstrap")
    aggregate = json.loads(LEGACY_BOOTSTRAP_SOURCE.read_text(encoding="utf-8"))
    SOURCE_DIR.mkdir(parents=True, exist_ok=True)
    buckets: dict[str, list[dict[str, object]]] = {name: [] for name in DOMAINS}
    for order, original in enumerate(aggregate["methods"]):
        method = dict(original)
        method["_order"] = order
        buckets[domain_for(str(method["name"]))].append(method)
    for domain in DOMAINS:
        write_contract_file(
            SOURCE_DIR / f"{domain}.json",
            formatted({"domain": domain, "methods": buckets[domain]}),
        )
    write_contract_file(
        SOURCE_DIR / "Manifest.json",
        formatted(
            {
                "apiVersion": aggregate["apiVersion"],
                "compatibility": aggregate.get("compatibility", {}),
                "events": aggregate.get("events", []),
            }
        ),
    )


def load_aggregate() -> dict[str, object]:
    manifest_path = SOURCE_DIR / "Manifest.json"
    if not manifest_path.exists():
        raise RuntimeError("Missing docs/contracts/native/Manifest.json; run --bootstrap once")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    # Schemas are authored once beside the domain method manifests and copied
    # verbatim into the generated contract. A future frontend consumes these
    # stable machine shapes instead of maintaining a handwritten interpretation.
    if not SCHEMAS.exists():
        raise RuntimeError("Missing docs/contracts/native/Schemas.json")
    schemas = json.loads(SCHEMAS.read_text(encoding="utf-8"))
    if not isinstance(schemas, dict):
        raise RuntimeError("Native schemas must be a JSON object")
    ordered: list[tuple[int, dict[str, object]]] = []
    names: set[str] = set()
    for domain in DOMAINS:
        path = SOURCE_DIR / f"{domain}.json"
        source = json.loads(path.read_text(encoding="utf-8"))
        if source.get("domain") != domain or not isinstance(source.get("methods"), list):
            raise RuntimeError(f"Invalid domain contract: {path.relative_to(ROOT)}")
        for original in source["methods"]:
            method = dict(original)
            order = method.pop("_order", None)
            method.setdefault("error", "NativeError")
            name = method.get("name")
            if not isinstance(order, int) or not isinstance(name, str):
                raise RuntimeError(f"Method in {path.name} needs string name and integer _order")
            if name in names:
                raise RuntimeError(f"Duplicate Native API method: {name}")
            if domain_for(name) != domain:
                raise RuntimeError(f"{name} belongs to {domain_for(name)}, not {domain}")
            names.add(name)
            ordered.append((order, method))
    ordered.sort(key=lambda item: item[0])
    if [order for order, _ in ordered] != list(range(len(ordered))):
        raise RuntimeError("Native method _order values must be contiguous from zero")
    return {
        "apiVersion": manifest["apiVersion"],
        "compatibility": manifest.get("compatibility", {}),
        "methods": [method for _, method in ordered],
        "events": manifest.get("events", []),
        "schemas": schemas,
    }


SCHEMA_KEYWORDS = {
    "$ref", "$comment", "type", "properties", "required", "additionalProperties", "patternProperties",
    "propertyNames", "minProperties", "maxProperties", "items", "minItems", "maxItems", "uniqueItems", "contains",
    "oneOf", "anyOf", "allOf", "not", "if", "then", "else", "enum", "const", "pattern", "format", "minLength",
    "maxLength", "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "multipleOf", "title",
    "description", "default", "examples",
}


def is_alias_map(schema: object) -> bool:
    """A schema written as {"key": "wire alias", ...} instead of JSON Schema.

    Every value is a string, and some key is not a JSON Schema keyword: an
    object may well have a property named "description" or "format"."""
    return (isinstance(schema, dict) and bool(schema)
            and all(isinstance(value, str) for value in schema.values())
            and not set(schema) <= SCHEMA_KEYWORDS)


def authored_params(method: dict[str, object], schemas: dict[str, object]) -> tuple[dict[str, bool], str | None]:
    """A method's authored request keys, each with whether it is optional.

    Returns the keys, or a reason the authored params cannot be read as keys."""
    shape = method.get("params")
    if shape is None:
        return {}, None
    if isinstance(shape, dict):
        return {
            key: isinstance(alias, str) and "undefined" in alias
            for key, alias in shape.items()
        }, None
    if isinstance(shape, str):
        alias = shape.strip()
        partial = alias.startswith("Partial<") and alias.endswith(">")
        if partial:
            alias = alias[len("Partial<"):-1].strip()
        schema = schemas.get(alias)
        seen: set[str] = set()
        while isinstance(schema, dict) and "$ref" in schema and schema["$ref"] not in seen:
            seen.add(schema["$ref"])
            schema = schemas.get(str(schema["$ref"]).removeprefix("#/schemas/"))
        if not isinstance(schema, dict) or not isinstance(schema.get("properties"), dict):
            return {}, f"its params {shape!r} name no schema with properties"
        required = set(schema.get("required", [])) if not partial else set()
        return {key: key not in required for key in schema["properties"]}, None
    return {}, f"its params {shape!r} are neither keys nor a schema name"


def param_drift(name: str, declared: dict[str, bool], registrations: list) -> list[str]:
    """How a method's authored params differ from what its handler reads.

    - a key the Bridge reads must be declared;
    - a declared key must be read (or, where the handler reads keys by a
      variable name, appear as a literal in the code it runs);
    - a key the contract calls optional must not be required by the Bridge,
      and one it calls required must not be read only as optional;
    - params handed somewhere the check cannot follow is drift in itself, so
      the check never passes by not looking."""
    drift: list[str] = []
    keys: dict[str, set[str]] = {}
    dynamic = False
    literals: set[str] = set()
    for registration in registrations:
        for key, kinds in registration.keys.items():
            keys.setdefault(key, set()).update(kinds)
        dynamic = dynamic or registration.dynamic
        literals |= registration.literals
        for callee in registration.unresolved:
            drift.append(f"Native method params: {name} hands params to {callee}, which the check cannot follow")
    for key, kinds in sorted(keys.items()):
        if key not in declared:
            drift.append(f"Native method params: {name} reads '{key}', which its contract does not declare")
        elif declared[key] and kinds == {"required"}:
            drift.append(f"Native method params: {name}.{key} is optional in its contract, but the Bridge requires it")
        elif not declared[key] and kinds == {"optional"}:
            drift.append(
                f"Native method params: {name}.{key} is required in its contract, but the Bridge reads it as optional")
    for key in sorted(declared):
        if key not in keys and not (dynamic and key in literals):
            drift.append(f"Native method params: {name} declares '{key}', which the Bridge never reads")
    return drift


def verify_implementation(contract: dict[str, object], backend_root: Path = BACKEND_ROOT) -> list[str]:
    bridge = "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted((backend_root / "Bridge").rglob("*.cpp"))
    )
    # What the Bridge really registers and what each handler reads, from the
    # code itself rather than from any mention of a name in it.
    registrations = bridge_reads.read_bridge(backend_root)
    missing: list[str] = []
	# Every named wire alias must resolve to an authored schema. Primitive,
	# literal and generic wrapper syntax remains inline.
    schemas = contract.get("schemas", {})
    aliases: set[str] = set()
    for method in contract["methods"]:
        for shape in (method.get("params"), method.get("result"), method.get("error")):
            values = [shape] if isinstance(shape, str) else list(shape.values()) if isinstance(shape, dict) else []
            for value in values:
                if not isinstance(value, str):
                    continue
                aliases.update(re.findall(r"\b[A-Z][A-Za-z0-9]*\b", value))
    aliases.discard("Partial")
    for alias in sorted(aliases):
        if alias not in schemas:
            missing.append(f"Native schema: {alias}")
    for name, schema in schemas.items():
        for reference in re.findall(r'#/schemas/([A-Za-z0-9]+)', json.dumps(schema)):
            if reference not in schemas:
                missing.append(f"Native schema reference: {name} -> {reference}")
    # One schema language: every authored schema is JSON Schema, so the
    # runtime parity acceptance and a client that validates against the
    # contract read every shape alike. A map of keys to wire aliases
    # ({"id": "string"}) is read by no validator: it checks nothing.
    for name, schema in schemas.items():
        if is_alias_map(schema):
            missing.append(f"Native schema: {name} is written as a map of aliases; write it as JSON Schema")
    # Every contract method has a real register_handler("name", ...) call. A
    # mention of the name anywhere else - the embedded client's invoke('name'),
    # a comment, a commented-out registration - serves no request.
    for method in contract["methods"]:
        name = method["name"]
        if name not in registrations:
            missing.append(f'C++ Bridge registration: {name} has no register_handler("{name}", ...)')
    # Full static method-surface parity is bidirectional: the check above
    # only ever asks "does every CONTRACT method have a real handler?" - it
    # says nothing about a real register_handler(...) call that has NO
    # contract entry at all (a real, live, undocumented capability - exactly
    # how window.prepareMain went unnoticed: registered and wired into the
    # embedded JS client, invisible to this generator, because nothing ever
    # checked this direction). Also catches the same method name registered
    # twice by mistake, which the contract-side duplicate check (
    # load_aggregate() above) cannot see since it only ever looks at the
    # authored contract files, never the real dispatch table.
    contract_names = {method["name"] for method in contract["methods"]}
    for registered, entries in sorted(registrations.items()):
        if registered not in contract_names:
            missing.append(f"Native contract entry: {registered} has a real C++ Bridge registration but no contract entry")
        if len(entries) > 1:
            missing.append(f"C++ Bridge registration: {registered} is registered {len(entries)} times (duplicate handler)")
    # The embedded JS client (bridge_script(), NativeBridge.cpp) is the
    # actual product surface a real lawyer's renderer calls through - every
    # contract method (worker or ui: invoke() is policy-agnostic, only the
    # backend dispatch differs) must have its own invoke('method.name'
    # real call site, not merely appear as a substring anywhere in the
    # 5000+ line Bridge source (the weaker check the "C++ Bridge
    # registration" line above already performs).
    js_invoked_names = set(re.findall(r"invoke\('([a-zA-Z][a-zA-Z0-9.]*)'", bridge))
    for method in contract["methods"]:
        name = method["name"]
        if name not in js_invoked_names:
            missing.append(f"JS client invoke() call: {name} has no real invoke('{name}') call site in bridge_script()")
    # A method's real dispatch policy - whether the frontend calls it and
    # gets a worker-thread response, or it can only run paired with a real
    # native dialog/window on the UI thread - comes from its own
    # register_handler(...) call, the one place that is actually true. Each
    # contract method must declare a matching "policy" so a reader (or this
    # parity tool's own real acceptance walk) never has to guess it from the
    # C++ source; a method whose registration disagrees with - or is missing
    # from - its own contract entry is exactly the kind of drift this
    # function exists to catch (Consilium.json shipped for a long time with
    # no "policy" field at all on 6 real, already-registered methods).
    policy_from_wire = {"worker": "Worker", "ui": "UiThread"}
    for method in contract["methods"]:
        name = method["name"]
        declared = method.get("policy")
        if declared not in policy_from_wire:
            missing.append(f"Native method policy: {name} has no valid \"policy\" (worker|ui)")
            continue
        if name not in registrations:
            continue  # already reported above as a missing C++ Bridge registration
        for registration in registrations[name]:
            if registration.policy != policy_from_wire[declared]:
                missing.append(
                    f"Native method policy: {name} is declared \"{declared}\" but registered "
                    f"ExecutionPolicy::{registration.policy}")
    # A method's params are what its handler reads (API-7: parameter
    # optionality is preserved). The authored params, the Bridge's reads and
    # each key's optionality must agree, or a client that follows the
    # contract sends what the Bridge ignores, or omits what it requires.
    for method in contract["methods"]:
        name = method["name"]
        if name not in registrations:
            continue
        declared_keys, unreadable = authored_params(method, contract.get("schemas", {}))
        if unreadable is not None:
            missing.append(f"Native method params: {name}: {unreadable}")
            continue
        missing.extend(param_drift(name, declared_keys, registrations[name]))
    # Events: every event Manifest.json declares is published by something,
    # and everything published is declared, so a page that listens for one
    # hears it, and none arrives unannounced.
    declared_events = set(contract.get("events", []))
    published = bridge_reads.published_events(backend_root)
    for event in sorted(declared_events - published):
        missing.append(f"Native event: {event} is declared in Manifest.json, but nothing publishes it")
    for event in sorted(published - declared_events):
        missing.append(f"Native event: {event} is published, but Manifest.json does not declare it")
    # The Bridge defines apiVersion exactly once, as kNativeApiVersion (see
    # NativeBridge.cpp) - both the embedded JS client and the runtime.getInfo
    # wire response read that single constant, so they cannot independently
    # drift from each other. This checks the ONE definition's value against
    # the contract's own apiVersion, rather than scanning for a literal
    # numeric substring that a refactor could silently stop matching.
    api_version = contract["apiVersion"]
    version_match = re.search(r"kNativeApiVersion\s*=\s*(\d+)\s*;", bridge)
    if not version_match:
        missing.append("C++ API version: kNativeApiVersion is not defined in the Bridge")
    elif int(version_match.group(1)) != api_version:
        missing.append(
            f"C++ API version: kNativeApiVersion is {version_match.group(1)}, contract apiVersion is {api_version}")
    # A THIRD independent place this same number can drift: Schemas.json's
    # own RuntimeInfo.apiVersion asserts the exact value a real
    # runtime.getInfo response must return. A real, empirical parity run
    # against the actual compiled binary (native_contract_parity_acceptance.py)
    # is what caught this copy still hardcoded at the pre-Phase-28 value (38)
    # after both C++ definitions above had already been fixed and verified -
    # source inspection alone had missed it.
    runtime_info_schema = contract.get("schemas", {}).get("RuntimeInfo", {})
    schema_api_version = runtime_info_schema.get("properties", {}).get("apiVersion", {}).get("const")
    if schema_api_version != api_version:
        missing.append(
            f"Native schema: Schemas.json RuntimeInfo.properties.apiVersion.const is {schema_api_version!r}, "
            f"contract apiVersion is {api_version}")
    contract_names_for_client = {method["name"] for method in contract["methods"]}
    for function, method_name in contract.get("client", {}).items():
        if method_name not in contract_names_for_client:
            missing.append(f"Embedded client: {function} calls {method_name}, which the contract does not declare")
    for global_name, members in contract.get("compatibility", {}).items():
        object_name = global_name.removeprefix("window.")
        if f"'{object_name}'" not in bridge:
            missing.append(f"C++ compatibility object: {global_name}")
        for member in members:
            if member not in bridge:
                missing.append(f"Compatibility member: {global_name}.{member}")
    return missing


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bootstrap", action="store_true")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.bootstrap:
        bootstrap()
    contract = load_aggregate()
    # The embedded client's function for each method, read from the Bridge's
    # own script, so a page (and a test's stand-in for the backend,
    # code/frontend/App/services/nativeContract.gate.js) knows which method a
    # window.nativeApp function calls.
    contract["client"] = bridge_reads.embedded_client(BACKEND_ROOT)
    output = formatted(contract)
    missing = verify_implementation(contract)
    if missing:
        print("Native API contract drift:", file=sys.stderr)
        for item in missing:
            print(f"  - {item}", file=sys.stderr)
        return 1
    if args.check:
        drift = [
            path.relative_to(ROOT)
            for path in (GENERATED, COMPATIBILITY_OUTPUT)
            if not path.exists() or path.read_bytes() != output.encode("utf-8")
        ]
        if drift:
            print("Generated Native API is stale:", file=sys.stderr)
            for path in drift:
                print(f"  - {path}", file=sys.stderr)
            return 1
    else:
        GENERATED.parent.mkdir(parents=True, exist_ok=True)
        write_contract_file(GENERATED, output)
        write_contract_file(COMPATIBILITY_OUTPUT, output)
    print(f"Native API v{contract['apiVersion']}: {len(contract['methods'])} methods verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
