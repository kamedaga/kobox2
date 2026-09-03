#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

import argparse
import hashlib
import json
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCHEMAS = (
    (ROOT / "protocol" / "schema" / "transport.json",
     ROOT / "protocol" / "generated" / "include" / "kobox2" / "protocol_layout.h",
     "KB2_PROTOCOL", "KOBOX2_PROTOCOL_LAYOUT_H"),
    (ROOT / "protocol" / "schema" / "closure.json",
     ROOT / "protocol" / "generated" / "include" / "kobox2" / "closure_layout.h",
     "KB2_CLOSURE", "KOBOX2_CLOSURE_LAYOUT_H"),
    (ROOT / "protocol" / "schema" / "resource_grant.json",
     ROOT / "protocol" / "generated" / "include" / "kobox2" / "resource_grant_layout.h",
     "KB2_RESOURCE_GRANT", "KOBOX2_RESOURCE_GRANT_LAYOUT_H"),
    (ROOT / "protocol" / "schema" / "gpu.json",
     ROOT / "protocol" / "generated" / "include" / "kobox2" / "gpu_layout.h",
     "KB2_GPU", "KOBOX2_GPU_LAYOUT_H"),
    (ROOT / "protocol" / "schema" / "gpu_drm_core.json",
     ROOT / "protocol" / "generated" / "include" / "kobox2" / "gpu_drm_core_layout.h",
     "KB2_GPU_DRM_CORE", "KOBOX2_GPU_DRM_CORE_LAYOUT_H"),
    (ROOT / "protocol" / "schema" / "gpu_drm_mode.json",
     ROOT / "protocol" / "generated" / "include" / "kobox2" / "gpu_drm_mode_layout.h",
     "KB2_GPU_DRM_MODE", "KOBOX2_GPU_DRM_MODE_LAYOUT_H"),
    (ROOT / "protocol" / "schema" / "gpu_drm_virtgpu.json",
     ROOT / "protocol" / "generated" / "include" / "kobox2" / "gpu_drm_virtgpu_layout.h",
     "KB2_GPU_DRM_VIRTGPU", "KOBOX2_GPU_DRM_VIRTGPU_LAYOUT_H"),
    (ROOT / "protocol" / "schema" / "gpu_drm_amdgpu.json",
     ROOT / "protocol" / "generated" / "include" / "kobox2" / "gpu_drm_amdgpu_layout.h",
     "KB2_GPU_DRM_AMDGPU", "KOBOX2_GPU_DRM_AMDGPU_LAYOUT_H"),
)

TYPE_SIZE = {
    "u8": 1,
    "i16": 2,
    "u16": 2,
    "i32": 4,
    "u32": 4,
    "i64": 8,
    "u64": 8,
}

ATTACHMENT_CLASSES = {
    "memory": 1,
    "dma_buf": 2,
    "sync_file": 3,
    "event": 4,
    "session": 5,
    "syncobj": 6,
}

ATTACHMENT_OWNERSHIP = {
    "borrow": 1,
    "share": 2,
    "move": 3,
}

DIRECTIONS = {
    "input": 1,
    "output": 2,
    "input_output": 3,
}

JSON_SAFE_INTEGER_MAX = 9007199254740991


def identifier(value):
    return re.sub(r"[^A-Za-z0-9]", "_", value).upper()


def append_macro_catalog(lines, name, entries):
    lines.append(f"#define {name}(X) \\")
    for index, entry in enumerate(entries):
        continuation = " \\" if index + 1 < len(entries) else ""
        lines.append(f"    X({', '.join(entry)}){continuation}")
    lines.append("")


def field_size(type_name):
    match = re.fullmatch(r"(u8|i16|u16|i32|u32|i64|u64)(?:\[(\d+)\])?", type_name)
    if match is None:
        raise ValueError(f"unsupported field type: {type_name}")
    count = int(match.group(2) or "1")
    if count == 0:
        raise ValueError(f"zero-length field type: {type_name}")
    return TYPE_SIZE[match.group(1)] * count


def schema_name(value, description):
    if not isinstance(value, str) or not re.fullmatch(r"[a-z][a-z0-9_]*", value):
        raise ValueError(f"invalid {description}")
    return value


def validate_records(schema):
    records = schema.get("records", [])
    record_names = set()
    record_ids = set()
    chunk_ids = []

    if not isinstance(records, list):
        raise ValueError("records must be a list")
    for index, record in enumerate(records):
        record_id = record.get("id")
        name = schema_name(record.get("name"), "record name")
        fields = record.get("fields")
        field_names = set()

        if (not isinstance(record_id, int) or record_id == 0 or
                record_id in record_ids or name in record_names):
            raise ValueError(f"invalid record identity: {name}")
        if index != 0 and record_id != records[index - 1]["id"] + 1:
            raise ValueError("record IDs must be a complete canonical sequence")
        if not isinstance(fields, list) or not fields:
            raise ValueError(f"record has no fields: {name}")
        for field in fields:
            field_name = schema_name(field.get("name"), "record field name")
            if field_name in field_names:
                raise ValueError(f"duplicate record field: {name}.{field_name}")
            field_names.add(field_name)
            field_size(field.get("type"))
        chunk_id = record.get("uapi_chunk_id")
        if chunk_id is not None:
            if not isinstance(chunk_id, int) or chunk_id == 0 or chunk_id in chunk_ids:
                raise ValueError(f"invalid UAPI chunk ID: {name}")
            chunk_ids.append(chunk_id)
        record_ids.add(record_id)
        record_names.add(name)

    constants = schema.get("constants", {})
    if "record_count" in constants and constants["record_count"] != len(records):
        raise ValueError("record count mismatch")
    if "cs_chunk_count" in constants:
        if (constants["cs_chunk_count"] != len(chunk_ids) or
                sorted(chunk_ids) != list(range(1, len(chunk_ids) + 1))):
            raise ValueError("CS chunk catalog mismatch")
    return {record["name"]: record for record in records}


def validate_contract_side(side, record_by_name, inline_argument_id, description):
    if not isinstance(side, dict):
        raise ValueError(f"invalid {description} contract")
    inline_record = side.get("inline_record")
    spans = side.get("spans")
    attachments = side.get("attachments")
    argument_ids = set()
    span_roles = set()
    attachment_roles = set()
    repeated_seen = False

    if inline_record is not None:
        if inline_record not in record_by_name:
            raise ValueError(f"unknown inline record: {description}.{inline_record}")
        argument_ids.add(inline_argument_id)
    if not isinstance(spans, list) or not isinstance(attachments, list):
        raise ValueError(f"invalid {description} span or attachment catalog")
    for index, span in enumerate(spans):
        argument_id = span.get("argument_id")
        role = span.get("role")
        name = schema_name(span.get("name"), "span name")
        direction = span.get("direction")
        records = span.get("records")
        minimum = span.get("minimum")
        maximum = span.get("maximum")
        repeated = span.get("repeated", False)
        element_minimum = span.get("element_minimum")
        element_maximum = span.get("element_maximum")

        if (not isinstance(argument_id, int) or argument_id == 0 or
                argument_id in argument_ids or not isinstance(role, int) or role == 0 or
                role in span_roles):
            raise ValueError(f"invalid span identity: {description}.{name}")
        if direction not in DIRECTIONS:
            raise ValueError(f"invalid span direction: {description}.{name}")
        if (not isinstance(records, list) or not records or
                len(records) != len(set(records)) or
                any(record not in record_by_name for record in records)):
            raise ValueError(f"invalid span records: {description}.{name}")
        if (not isinstance(minimum, int) or not isinstance(maximum, int) or
                minimum < 0 or maximum < minimum or maximum == 0):
            raise ValueError(f"invalid span bounds: {description}.{name}")
        if not isinstance(repeated, bool) or (repeated and (repeated_seen or
                index != len(spans) - 1 or attachments)):
            raise ValueError(f"invalid repeated span: {description}.{name}")
        if repeated:
            if (not isinstance(element_minimum, int) or
                    not isinstance(element_maximum, int) or element_minimum < 1 or
                    element_maximum < element_minimum):
                raise ValueError(f"invalid repeated span element bounds: {description}.{name}")
        elif element_minimum is not None or element_maximum is not None:
            raise ValueError(f"unexpected span element bounds: {description}.{name}")
        repeated_seen = repeated_seen or repeated
        argument_ids.add(argument_id)
        span_roles.add(role)
    for attachment in attachments:
        argument_id = attachment.get("argument_id")
        role = attachment.get("role")
        name = schema_name(attachment.get("name"), "attachment name")
        class_name = attachment.get("class")
        class_names = attachment.get("classes")
        ownership = attachment.get("ownership")
        optional = attachment.get("optional")

        if (not isinstance(argument_id, int) or argument_id == 0 or
                argument_id in argument_ids or not isinstance(role, int) or role == 0 or
                role in attachment_roles):
            raise ValueError(f"invalid attachment identity: {description}.{name}")
        if (class_name is None) == (class_names is None):
            raise ValueError(f"attachment must select class or classes: {description}.{name}")
        if class_name is not None:
            class_names = [class_name]
        if (not isinstance(class_names, list) or not class_names or
                len(class_names) != len(set(class_names)) or
                any(value not in ATTACHMENT_CLASSES for value in class_names)):
            raise ValueError(f"invalid attachment classes: {description}.{name}")
        if ownership not in ATTACHMENT_OWNERSHIP or not isinstance(optional, bool):
            raise ValueError(f"invalid attachment disposition: {description}.{name}")
        argument_ids.add(argument_id)
        attachment_roles.add(role)


def validate_command_contracts(schema, record_by_name):
    command_set = schema["command_set"]
    commands = command_set["commands"]
    has_contracts = any("request" in command or "completion" in command
                        for command in commands)
    uapi_commands = set()
    uapi_numbers = set()

    if not has_contracts:
        if record_by_name or schema.get("info_queries"):
            raise ValueError("records require command contracts")
        return
    inline_argument_id = command_set.get("inline_argument_id")
    if not isinstance(inline_argument_id, int) or inline_argument_id == 0:
        raise ValueError("invalid inline argument ID")
    for command in commands:
        name = command["name"]
        uapi_command = command.get("uapi_command")
        uapi_number = command.get("uapi_number")
        deadline = command.get("deadline")

        if (not isinstance(uapi_command, str) or
                not re.fullmatch(r"DRM_IOCTL_[A-Z0-9_]+", uapi_command) or
                uapi_command in uapi_commands):
            raise ValueError(f"invalid UAPI command: {name}")
        if (not isinstance(uapi_number, int) or uapi_number < 0 or
                uapi_number in uapi_numbers):
            raise ValueError(f"invalid UAPI command number: {name}")
        if ((command["cancellation"] == "wait" and deadline != "required") or
                (command["cancellation"] != "wait" and deadline != "forbidden")):
            raise ValueError(f"invalid deadline policy: {name}")
        validate_contract_side(command.get("request"),
                               record_by_name,
                               inline_argument_id,
                               f"{name}.request")
        validate_contract_side(command.get("completion"),
                               record_by_name,
                               inline_argument_id,
                               f"{name}.completion")
        uapi_commands.add(uapi_command)
        uapi_numbers.add(uapi_number)


def validate_info_queries(schema, record_by_name):
    queries = schema.get("info_queries", [])
    query_ids = set()
    query_names = set()

    if not isinstance(queries, list):
        raise ValueError("info queries must be a list")
    for query in queries:
        query_id = query.get("id")
        name = schema_name(query.get("name"), "info query name")
        records = query.get("records")
        if (not isinstance(query_id, int) or query_id < 0 or query_id in query_ids or
                name in query_names or not isinstance(records, list) or not records or
                len(records) != len(set(records)) or
                any(record not in record_by_name for record in records) or
                query.get("cardinality") not in ("one", "vector", "variant")):
            raise ValueError(f"invalid info query: {name}")
        query_ids.add(query_id)
        query_names.add(name)
    constants = schema.get("constants", {})
    if "info_query_count" in constants and constants["info_query_count"] != len(queries):
        raise ValueError("info query count mismatch")


def validate_canonical_value(value):
    if value is None or isinstance(value, bool):
        return
    if isinstance(value, int):
        if abs(value) > JSON_SAFE_INTEGER_MAX:
            raise ValueError("integer is outside the RFC 8785 exact range")
        return
    if isinstance(value, str):
        try:
            value.encode("ascii")
        except UnicodeEncodeError as error:
            raise ValueError("schema strings must be ASCII") from error
        return
    if isinstance(value, list):
        for element in value:
            validate_canonical_value(element)
        return
    if isinstance(value, dict):
        for key, element in value.items():
            if not isinstance(key, str):
                raise ValueError("schema object keys must be strings")
            validate_canonical_value(key)
            validate_canonical_value(element)
        return
    raise ValueError(f"unsupported schema value: {type(value).__name__}")


def load_schema(schema_path):
    with schema_path.open("r", encoding="utf-8") as source:
        schema = json.load(source)
    if schema.get("spdx") != "MIT":
        raise ValueError("schema SPDX must be MIT")
    if schema.get("byte_order") != "little":
        raise ValueError("only little-endian schemas are supported")
    identity = schema.get("abi_identity", "").encode("ascii")
    if identity != b"dev\0":
        raise ValueError("development ABI identity must be dev\\0")
    if schema.get("digest_algorithm") != "sha256":
        raise ValueError("schema digest must use SHA-256")
    if schema.get("digest_canonicalization") != "RFC8785":
        raise ValueError("schema digest must use RFC 8785 canonicalization")
    validate_canonical_value(schema)
    record_by_name = validate_records(schema)

    command_set = schema.get("command_set")
    if command_set is not None:
        constants = schema["constants"]
        commands = command_set["commands"]
        if schema.get("base_schema") != "kobox2.gpu":
            raise ValueError("command set must use the kobox2.gpu base schema")
        if command_set["set_id"] == 0 or constants.get("set_id") != command_set["set_id"]:
            raise ValueError("command set ID mismatch")
        if command_set["queue_class"] not in ("display", "execution"):
            raise ValueError("invalid command set queue class")
        if command_set["argument_model"] != "kobox2.gpu.argument-vector":
            raise ValueError("invalid command set argument model")
        if schema.get("uapi_release") != "Linux 6.18.48":
            raise ValueError("command set UAPI release mismatch")
        if not schema.get("uapi_headers"):
            raise ValueError("command set must identify its UAPI headers")
        if constants.get("command_count") != len(commands):
            raise ValueError("command count mismatch")
        for index, command in enumerate(commands, start=1):
            if command.get("id") != index:
                raise ValueError("command IDs must be a complete canonical sequence")
            name = schema_name(command.get("name"), "command name")
            if constants.get(f"command_{name}") != index:
                raise ValueError(f"command constant mismatch: {name}")
            if command.get("cancellation") not in ("before-dispatch", "wait"):
                raise ValueError(f"invalid cancellation policy: {name}")
        validate_command_contracts(schema, record_by_name)
    elif record_by_name:
        raise ValueError("records require a command set")

    validate_info_queries(schema, record_by_name)

    for structure_name, structure in schema["structures"].items():
        structure_size = structure["size"]
        occupied = [False] * structure_size
        names = set()
        for field in structure["fields"]:
            name = field["name"]
            if name in names:
                raise ValueError(f"duplicate field {structure_name}.{name}")
            names.add(name)
            start = field["offset"]
            end = start + field_size(field["type"])
            if start < 0 or end > structure_size:
                raise ValueError(f"field outside {structure_name}: {name}")
            if any(occupied[start:end]):
                raise ValueError(f"overlapping field in {structure_name}: {name}")
            occupied[start:end] = [True] * (end - start)
        if not all(occupied):
            raise ValueError(f"unassigned bytes in {structure_name}")
    return schema


def validate_gpu_family(schemas):
    base = schemas["kobox2.gpu"]
    command_sets = base.get("command_sets", [])
    profiles = base.get("profiles", [])
    set_ids = set()
    referenced_set_ids = set()

    if base.get("uapi_release") != "Linux 6.18.48":
        raise ValueError("GPU base UAPI release mismatch")
    for name, value in ATTACHMENT_CLASSES.items():
        if base["constants"].get(f"attachment_{name}") != value:
            raise ValueError("GPU attachment class catalog mismatch")
    for descriptor in command_sets:
        set_id = descriptor.get("set_id")
        command_schema = schemas.get(descriptor.get("schema"))
        if set_id in set_ids or set_id == 0 or command_schema is None:
            raise ValueError("invalid GPU command-set catalog")
        if command_schema["command_set"]["set_id"] != set_id:
            raise ValueError("GPU command-set catalog ID mismatch")
        if command_schema.get("uapi_release") != base["uapi_release"]:
            raise ValueError("GPU command-set UAPI release mismatch")
        set_ids.add(set_id)
    if len(set_ids) != 4:
        raise ValueError("GPU command-set catalog must contain four sets")

    profile_ids = set()
    for profile in profiles:
        profile_kind = profile.get("profile_kind")
        name = profile.get("name")
        profile_sets = profile.get("command_sets")
        if (profile_kind in profile_ids or profile_kind == 0 or
                base["constants"].get(f"profile_{name}") != profile_kind or
                not isinstance(profile_sets, list) or len(profile_sets) != len(set(profile_sets)) or
                any(set_id not in set_ids for set_id in profile_sets)):
            raise ValueError("invalid GPU profile catalog")
        profile_ids.add(profile_kind)
        referenced_set_ids.update(profile_sets)
    if len(profile_ids) != 2 or referenced_set_ids != set_ids:
        raise ValueError("GPU profile coverage mismatch")


def render(schema, prefix, guard):
    canonical = json.dumps(schema, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    digest = hashlib.sha256(canonical.encode("utf-8")).digest()
    digest_lines = []
    for index in range(0, len(digest), 8):
        values = ", ".join(f"0x{value:02x}" for value in digest[index : index + 8])
        if index + 8 < len(digest):
            values += ","
        digest_lines.append(f"        {values} \\")

    lines = [
        "/* SPDX-License-Identifier: MIT */",
        "/* Generated by tools/generate_protocol.py. Do not edit. */",
        "",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        f"#define {prefix}_SCHEMA_SHA256_HEX \\",
        f'    "{digest.hex()[:32]}" ' + "\\",
        f'    "{digest.hex()[32:]}"',
        f"#define {prefix}_SCHEMA_DIGEST_SIZE 32u",
        f"#define {prefix}_SCHEMA_SHA256_BYTES \\",
        "    { \\",
        *digest_lines,
        "    }",
        "",
        f"#define {prefix}_ABI_IDENTITY_SIZE 4u",
        f"#define {prefix}_ABI_IDENTITY_BYTES {{0x64, 0x65, 0x76, 0x00}}",
        "",
    ]

    for name, value in schema["constants"].items():
        suffix = "ull" if value > 0xFFFFFFFF else "u"
        lines.append(f"#define {prefix}_{identifier(name)} {value}{suffix}")
    lines.append("")

    record_by_name = {record["name"]: record for record in schema.get("records", [])}
    for record in schema.get("records", []):
        record_prefix = f"{prefix}_RECORD_{identifier(record['name'])}"
        offset = 0

        lines.append(f"#define {record_prefix} {record['id']}u")
        lines.append(
            f"#define {record_prefix}_SIZE "
            f"{sum(field_size(field['type']) for field in record['fields'])}u"
        )
        if "uapi_chunk_id" in record:
            lines.append(
                f"#define {record_prefix}_UAPI_CHUNK_ID {record['uapi_chunk_id']}u"
            )
        for field in record["fields"]:
            lines.append(
                f"#define {record_prefix}_{identifier(field['name'])}_OFFSET {offset}u"
            )
            offset += field_size(field["type"])
        lines.append("")

    if schema.get("records"):
        append_macro_catalog(
            lines,
            f"{prefix}_RECORD_CATALOG",
            [(f"{prefix}_RECORD_{identifier(record['name'])}",
              f"{prefix}_RECORD_{identifier(record['name'])}_SIZE")
             for record in schema["records"]],
        )

    for query in schema.get("info_queries", []):
        lines.append(
            f"#define {prefix}_INFO_QUERY_{identifier(query['name'])} {query['id']}u"
        )
    if schema.get("info_queries"):
        lines.append("")

    command_set = schema.get("command_set")
    if command_set is not None and "inline_argument_id" in command_set:
        inline_argument_id = command_set["inline_argument_id"]
        lines.append(f"#define {prefix}_INLINE_ARGUMENT_ID {inline_argument_id}u")
        lines.append("")
        for command in command_set["commands"]:
            command_prefix = f"{prefix}_COMMAND_{identifier(command['name'])}"
            lines.append(
                f"#define {command_prefix}_UAPI_NUMBER {command['uapi_number']}u"
            )
            lines.append(
                f"#define {command_prefix}_INLINE_ARGUMENT_ID {inline_argument_id}u"
            )
            for side_name in ("request", "completion"):
                side = command[side_name]
                side_prefix = f"{command_prefix}_{identifier(side_name)}"
                inline_record = side["inline_record"]
                inline_id = 0 if inline_record is None else record_by_name[inline_record]["id"]
                lines.append(f"#define {side_prefix}_INLINE_RECORD {inline_id}u")
                lines.append(f"#define {side_prefix}_SPAN_COUNT {len(side['spans'])}u")
                lines.append(
                    f"#define {side_prefix}_ATTACHMENT_COUNT {len(side['attachments'])}u"
                )
                for span in side["spans"]:
                    span_prefix = f"{side_prefix}_SPAN_{identifier(span['name'])}"
                    lines.append(
                        f"#define {span_prefix}_ARGUMENT_ID {span['argument_id']}u"
                    )
                    lines.append(f"#define {span_prefix}_ROLE {span['role']}u")
                    lines.append(
                        f"#define {span_prefix}_DIRECTION {DIRECTIONS[span['direction']]}u"
                    )
                    lines.append(f"#define {span_prefix}_MINIMUM {span['minimum']}u")
                    lines.append(f"#define {span_prefix}_MAXIMUM {span['maximum']}u")
                    lines.append(
                        f"#define {span_prefix}_REPEATED "
                        f"{1 if span.get('repeated', False) else 0}u"
                    )
                    if span.get("repeated", False):
                        lines.append(
                            f"#define {span_prefix}_ELEMENT_MINIMUM "
                            f"{span['element_minimum']}u"
                        )
                        lines.append(
                            f"#define {span_prefix}_ELEMENT_MAXIMUM "
                            f"{span['element_maximum']}u"
                        )
                    lines.append(
                        f"#define {span_prefix}_RECORD_COUNT {len(span['records'])}u"
                    )
                    for index, record_name in enumerate(span["records"]):
                        lines.append(
                            f"#define {span_prefix}_RECORD_{index} "
                            f"{record_by_name[record_name]['id']}u"
                        )
                for attachment in side["attachments"]:
                    attachment_prefix = (
                        f"{side_prefix}_ATTACHMENT_{identifier(attachment['name'])}"
                    )
                    class_names = attachment.get("classes")
                    if class_names is None:
                        class_names = [attachment["class"]]
                    lines.append(
                        f"#define {attachment_prefix}_ARGUMENT_ID "
                        f"{attachment['argument_id']}u"
                    )
                    lines.append(f"#define {attachment_prefix}_ROLE {attachment['role']}u")
                    lines.append(
                        f"#define {attachment_prefix}_OWNERSHIP "
                        f"{ATTACHMENT_OWNERSHIP[attachment['ownership']]}u"
                    )
                    lines.append(
                        f"#define {attachment_prefix}_OPTIONAL "
                        f"{1 if attachment['optional'] else 0}u"
                    )
                    lines.append(
                        f"#define {attachment_prefix}_CLASS_COUNT {len(class_names)}u"
                    )
                    for index, class_name in enumerate(class_names):
                        lines.append(
                            f"#define {attachment_prefix}_CLASS_{index} "
                            f"{ATTACHMENT_CLASSES[class_name]}u"
                        )
            lines.append("")
        append_macro_catalog(
            lines,
            f"{prefix}_REQUEST_INLINE_CATALOG",
            [(f"{prefix}_COMMAND_{identifier(command['name'])}_REQUEST_INLINE_RECORD",
              (f"{prefix}_RECORD_"
               f"{identifier(command['request']['inline_record'])}_SIZE"
               if command["request"]["inline_record"] is not None else "0u"))
             for command in command_set["commands"]],
        )
        append_macro_catalog(
            lines,
            f"{prefix}_REQUEST_CONTRACT_CATALOG",
            [(str(command["id"]),
              f"{prefix}_COMMAND_{identifier(command['name'])}_REQUEST_INLINE_RECORD",
              (f"{prefix}_RECORD_"
               f"{identifier(command['request']['inline_record'])}_SIZE"
               if command["request"]["inline_record"] is not None else "0u"),
              str(len(command["request"]["spans"])),
              str(len(command["request"]["attachments"])))
             for command in command_set["commands"]],
        )
        request_span_entries = []
        request_attachment_entries = []
        for command in command_set["commands"]:
            for span in command["request"]["spans"]:
                for record_name in span["records"]:
                    record = record_by_name[record_name]
                    request_span_entries.append(
                        (str(command["id"]), str(span["argument_id"]),
                         str(DIRECTIONS[span["direction"]]), str(span["minimum"]),
                         str(span["maximum"]), str(record["id"]),
                         str(sum(field_size(field["type"])
                                 for field in record["fields"])))
                    )
            for attachment in command["request"]["attachments"]:
                class_names = attachment.get("classes")
                if class_names is None:
                    class_names = [attachment["class"]]
                for class_name in class_names:
                    request_attachment_entries.append(
                        (str(command["id"]), str(attachment["argument_id"]),
                         str(attachment["role"]),
                         str(ATTACHMENT_OWNERSHIP[attachment["ownership"]]),
                         "1" if attachment["optional"] else "0",
                         str(ATTACHMENT_CLASSES[class_name]))
                    )
        if request_span_entries:
            append_macro_catalog(
                lines, f"{prefix}_REQUEST_SPAN_CATALOG", request_span_entries
            )
        if request_attachment_entries:
            append_macro_catalog(
                lines, f"{prefix}_REQUEST_ATTACHMENT_CATALOG",
                request_attachment_entries,
            )
        append_macro_catalog(
            lines,
            f"{prefix}_COMPLETION_INLINE_CATALOG",
            [(f"{prefix}_COMMAND_{identifier(command['name'])}_COMPLETION_INLINE_RECORD",
              (f"{prefix}_RECORD_"
               f"{identifier(command['completion']['inline_record'])}_SIZE"
               if command["completion"]["inline_record"] is not None else "0u"))
             for command in command_set["commands"]],
        )
        append_macro_catalog(
            lines,
            f"{prefix}_COMPLETION_CONTRACT_CATALOG",
            [(str(command["id"]),
              f"{prefix}_COMMAND_{identifier(command['name'])}_COMPLETION_INLINE_RECORD",
              (f"{prefix}_RECORD_"
               f"{identifier(command['completion']['inline_record'])}_SIZE"
               if command["completion"]["inline_record"] is not None else "0u"),
              str(len(command["completion"]["spans"])),
              str(len(command["completion"]["attachments"])))
             for command in command_set["commands"]],
        )
        completion_attachment_entries = []
        for command in command_set["commands"]:
            for attachment in command["completion"]["attachments"]:
                class_names = attachment.get("classes")
                if class_names is None:
                    class_names = [attachment["class"]]
                for class_name in class_names:
                    completion_attachment_entries.append(
                        (str(command["id"]), str(attachment["argument_id"]),
                         str(attachment["role"]),
                         str(ATTACHMENT_OWNERSHIP[attachment["ownership"]]),
                         "1" if attachment["optional"] else "0",
                         str(ATTACHMENT_CLASSES[class_name]))
                    )
        if completion_attachment_entries:
            append_macro_catalog(
                lines, f"{prefix}_COMPLETION_ATTACHMENT_CATALOG",
                completion_attachment_entries,
            )

    for structure_name, structure in schema["structures"].items():
        structure_prefix = f"{prefix}_{identifier(structure_name)}"
        lines.append(f"#define {structure_prefix}_SIZE {structure['size']}u")
        for field in structure["fields"]:
            lines.append(
                f"#define {structure_prefix}_{identifier(field['name'])}_OFFSET "
                f"{field['offset']}u"
            )
        lines.append("")

    lines.extend(["#endif", ""])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()

    try:
        loaded = [(load_schema(schema), output, prefix, guard)
                  for schema, output, prefix, guard in SCHEMAS]
        validate_gpu_family({schema["name"]: schema for schema, _, _, _ in loaded})
        rendered = [(output, render(schema, prefix, guard))
                    for schema, output, prefix, guard in loaded]
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"protocol generation failed: {error}", file=sys.stderr)
        return 1

    if arguments.check:
        try:
            for output, expected in rendered:
                actual = output.read_text(encoding="utf-8")
                if actual != expected:
                    print(f"generated file is stale: {output}", file=sys.stderr)
                    return 1
        except OSError as error:
            print(f"protocol generation check failed: {error}", file=sys.stderr)
            return 1
        return 0

    for output_path, expected in rendered:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with output_path.open("w", encoding="utf-8", newline="\n") as output:
            output.write(expected)
    return 0


if __name__ == "__main__":
    sys.exit(main())
