#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

import argparse
import hashlib
import json
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
CORE_RUNTIME_ABI_HEADER = (
    ROOT / "protocol" / "generated" / "include" / "kobox2" /
    "core_runtime.h"
)
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
    (ROOT / "protocol" / "schema" / "memory_arena.json",
     ROOT / "protocol" / "generated" / "include" / "kobox2" / "memory_arena_layout.h",
     "KB2_MEMORY_ARENA", "KOBOX2_MEMORY_ARENA_LAYOUT_H"),
    (ROOT / "protocol" / "schema" / "pci_function.json",
     ROOT / "protocol" / "generated" / "include" / "kobox2" / "pci_function_layout.h",
     "KB2_PCI_FUNCTION", "KOBOX2_PCI_FUNCTION_LAYOUT_H"),
    (ROOT / "protocol" / "schema" / "dma_domain.json",
     ROOT / "protocol" / "generated" / "include" / "kobox2" / "dma_domain_layout.h",
     "KB2_DMA_DOMAIN", "KOBOX2_DMA_DOMAIN_LAYOUT_H"),
    (ROOT / "protocol" / "schema" / "irq_endpoint.json",
     ROOT / "protocol" / "generated" / "include" / "kobox2" / "irq_endpoint_layout.h",
     "KB2_IRQ_ENDPOINT", "KOBOX2_IRQ_ENDPOINT_LAYOUT_H"),
    (ROOT / "protocol" / "schema" / "core_runtime.json",
     ROOT / "protocol" / "generated" / "include" / "kobox2" / "core_runtime_layout.h",
     "KB2_CORE_RUNTIME", "KOBOX2_CORE_RUNTIME_LAYOUT_H"),
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


def validate_memory_arena(schema):
    constants = schema["constants"]

    if schema.get("resource_type") != "memory":
        raise ValueError("memory arena resource type mismatch")
    if constants != {
        "page_size": 4096,
        "minimum_length": 8192,
        "native_handle_role_memory": 0,
        "required_rights": 7,
        "operation_count": 1,
    }:
        raise ValueError("memory arena constants mismatch")
    if schema.get("native_handles") != [{
        "role": 0,
        "name": "memory",
        "count": 1,
    }]:
        raise ValueError("memory arena native handle contract mismatch")
    if schema.get("mapping") != {
        "alignment": 4096,
        "length_multiple": 4096,
        "lifetime": "resource-binding",
        "access": "shared-read-write",
    }:
        raise ValueError("memory arena mapping contract mismatch")
    if schema.get("linux_descriptor") != {
        "kind": "memfd",
        "close_on_exec": True,
        "seal_policy": "exact",
        "required_seals": ["seal", "shrink", "grow"],
    }:
        raise ValueError("memory arena Linux descriptor contract mismatch")
    if schema.get("operations") != [{
        "id": 1,
        "name": "mapped_range",
        "outputs": ["address", "length"],
    }]:
        raise ValueError("memory arena operation contract mismatch")


def validate_device_resource(schema, name, resource_type, native_handles,
                             required_rights, operation_names):
    constants = schema["constants"]
    operations = schema.get("operations")

    if schema.get("name") != name or schema.get("resource_type") != resource_type:
        raise ValueError(f"{name} resource identity mismatch")
    expected_handles = [
        {"role": role, "name": handle_name, "count": 1}
        for role, handle_name in native_handles
    ]
    if schema.get("native_handles") != expected_handles:
        raise ValueError(f"{name} native handle contract mismatch")
    for role, handle_name in native_handles:
        if constants.get(f"native_handle_role_{handle_name}") != role:
            raise ValueError(f"{name} native handle role mismatch")
    if constants.get("required_rights") != required_rights:
        raise ValueError(f"{name} required rights mismatch")
    if (not isinstance(operations, list) or
            constants.get("operation_count") != len(operation_names) or
            len(operations) != len(operation_names)):
        raise ValueError(f"{name} operation count mismatch")
    for operation_id, (operation, expected_name) in enumerate(
            zip(operations, operation_names), 1):
        if (operation.get("id") != operation_id or
                operation.get("name") != expected_name or
                not isinstance(operation.get("inputs", []), list) or
                not isinstance(operation.get("outputs", []), list)):
            raise ValueError(
                f"{name} operation contract mismatch: {expected_name}"
            )


def validate_core_runtime(schema):
    constants = schema["constants"]
    expected_interfaces = (
        (1, "memory", []),
        (2, "cpu", [1]),
        (3, "sync", [1, 2]),
        (4, "thread", [1, 2, 3]),
        (5, "time", [3, 4]),
        (6, "workqueue", [1, 2, 3, 4, 5]),
        (7, "rcu", [1, 2, 3, 4, 5, 6]),
    )
    interfaces = schema.get("interfaces")

    if constants.get("pointer_bits") != 64 or constants.get("size_bits") != 64:
        raise ValueError("core runtime data model mismatch")
    if constants.get("name_maximum_bytes") != 63:
        raise ValueError("core runtime name bound mismatch")
    if (constants.get("directory_size") != 56 or
            constants.get("binding_size") != 16 or
            constants.get("interface_header_size") != 48):
        raise ValueError("core runtime ABI size mismatch")
    if schema.get("abi") != {
        "data_model": "LP64",
        "calling_convention": "C",
        "pointer_bits": 64,
        "size_bits": 64,
        "status_bits": 32,
        "operation_slot_bits": 64,
        "directory_fields": [
            "size", "identity", "schema_digest", "bind", "unbind"],
        "interface_header_fields": [
            "size", "identity", "interface_id", "reserved", "schema_digest"],
        "interface_operation_order": "ascending-operation-id",
        "binding_fields": ["operations", "object"],
    }:
        raise ValueError("core runtime ABI layout mismatch")

    c_abi = schema.get("c_abi")
    if not isinstance(c_abi, dict) or {
            key: value for key, value in c_abi.items()
            if key != "semantic_types"} != {
                "operation_result": "status",
                "receiver": "binding_object",
                "output_passing": "pointer",
                "handle_representation": "pointer-to-incomplete-structure",
                "string_representation": "const-char-pointer-and-size",
                "cpu_mask_representation": "const-u64-pointer-and-word-count",
            }:
        raise ValueError("core runtime C ABI rules mismatch")
    semantic_types = c_abi.get("semantic_types")
    if not isinstance(semantic_types, dict):
        raise ValueError("core runtime C ABI semantic types are missing")

    if not isinstance(interfaces, list) or len(interfaces) != len(
            expected_interfaces):
        raise ValueError("core runtime interface count mismatch")
    if constants.get("interface_count") != len(expected_interfaces):
        raise ValueError("core runtime interface constant mismatch")
    for interface, (interface_id, name, dependencies) in zip(
            interfaces, expected_interfaces):
        if (interface.get("id") != interface_id or
                interface.get("name") != name or
                interface.get("dependencies") != dependencies or
                interface.get("receiver") != "binding" or
                constants.get(f"interface_{name}") != interface_id):
            raise ValueError(f"invalid core runtime interface: {name}")
        objects = interface.get("objects")
        operations = interface.get("operations")
        if (not isinstance(objects, list) or not objects or
                len(objects) != len(set(objects)) or
                any(schema_name(item, "core object name") != item
                    for item in objects) or
                not isinstance(operations, list) or not operations or
                constants.get(f"{name}_operation_count") != len(operations) or
                constants.get(f"{name}_table_size") != 48 + 8 * len(operations)):
            raise ValueError(f"invalid core runtime catalog: {name}")
        operation_names = set()
        for operation_id, operation in enumerate(operations, 1):
            operation_name = schema_name(
                operation.get("name"), "core operation name")
            if (operation.get("id") != operation_id or
                    operation_name in operation_names or
                    operation.get("blocking") not in ("never", "may") or
                    not isinstance(operation.get("inputs"), list) or
                    not isinstance(operation.get("outputs"), list) or
                    constants.get(
                        f"{name}_operation_{operation_name}") != operation_id):
                raise ValueError(
                    f"invalid core runtime operation: {name}.{operation_name}")
            operation_names.add(operation_name)

    directory = schema.get("directory_operations")
    if (not isinstance(directory, list) or
            constants.get("directory_operation_count") != len(directory)):
        raise ValueError("invalid core runtime directory")
    for operation_id, operation in enumerate(directory, 1):
        name = schema_name(operation.get("name"), "directory operation name")
        if (operation.get("id") != operation_id or
                operation.get("blocking") not in ("never", "may") or
                not isinstance(operation.get("inputs"), list) or
                not isinstance(operation.get("outputs"), list) or
                constants.get(f"directory_operation_{name}") != operation_id):
            raise ValueError(f"invalid core runtime directory operation: {name}")

    semantic_names = {
        item
        for operation in directory
        for side in ("inputs", "outputs")
        for item in operation[side]
    }
    semantic_names.update(
        item
        for interface in interfaces
        for operation in interface["operations"]
        for side in ("inputs", "outputs")
        for item in operation[side]
    )
    if set(semantic_types) != semantic_names:
        missing = sorted(semantic_names - set(semantic_types))
        extra = sorted(set(semantic_types) - semantic_names)
        raise ValueError(
            f"core runtime C ABI semantic type mismatch: "
            f"missing={missing}, extra={extra}"
        )
    callbacks_by_name = {item["name"] for item in schema.get("callbacks", [])}
    valid_plain_types = {
        "binding", "boolean", "cpu_mask", "i32", "module_context",
        "pointer", "schema_digest", "size", "string", "u32", "u64",
    }
    for name, type_name in semantic_types.items():
        if not isinstance(type_name, str):
            raise ValueError(f"invalid core runtime C ABI type: {name}")
        if type_name in valid_plain_types:
            continue
        if type_name.startswith("handle:"):
            schema_name(type_name.removeprefix("handle:"), "core handle type")
            continue
        if (type_name.startswith("callback:") and
                type_name.removeprefix("callback:") in callbacks_by_name):
            continue
        raise ValueError(f"invalid core runtime C ABI type: {name}")
    invalid_output_types = {
        "cpu_mask", "module_context", "schema_digest", "string",
    }
    for operation in [*directory, *(
            operation
            for interface in interfaces
            for operation in interface["operations"]
    )]:
        for output in operation["outputs"]:
            type_name = semantic_types[output]
            if (type_name in invalid_output_types or
                    type_name.startswith("callback:")):
                raise ValueError(
                    f"invalid core runtime C ABI output type: {output}"
                )

    statuses = schema.get("statuses")
    status_classes = {"success", "contract", "resource", "wait", "fault"}
    if not isinstance(statuses, list) or not statuses:
        raise ValueError("invalid core runtime status catalog")
    for status_id, status in enumerate(statuses):
        name = schema_name(status.get("name"), "core status name")
        if (status.get("id") != status_id or
                status.get("class") not in status_classes or
                constants.get(f"status_{name}") != status_id):
            raise ValueError(f"invalid core runtime status: {name}")

    callbacks = schema.get("callbacks")
    if callbacks != [
        {"name": "cache_constructor",
         "inputs": ["allocation", "object_size", "argument"],
         "returns": "status", "context": "caller"},
        {"name": "cache_destructor",
         "inputs": ["allocation", "object_size", "argument"],
         "returns": "void", "context": "caller"},
        {"name": "thread_entry", "inputs": ["argument"],
         "returns": "i32", "context": "thread"},
        {"name": "timer_callback",
         "inputs": ["timer", "argument", "expiration_count"],
         "returns": "void", "context": "timer-selected"},
        {"name": "work_callback", "inputs": ["work", "argument"],
         "returns": "void", "context": "worker"},
        {"name": "rcu_callback", "inputs": ["argument"],
         "returns": "void", "context": "worker"},
    ]:
        raise ValueError("core runtime callback catalog mismatch")

    inline = schema.get("inline_primitives")
    inline_types = [
        {"name": "atomic32", "size": 4, "alignment": 4},
        {"name": "atomic64", "size": 8, "alignment": 8},
        {"name": "refcount", "size": 4, "alignment": 4},
        {"name": "bitmap_word", "size": 8, "alignment": 8},
    ]
    memory_order_names = ["relaxed", "acquire", "release", "acq_rel", "seq_cst"]
    inline_operation_names = [
        "read_once", "write_once", "fence", "atomic_load", "atomic_store",
        "atomic_exchange", "atomic_compare_exchange", "atomic_fetch_add",
        "atomic_fetch_sub", "atomic_fetch_and", "atomic_fetch_or",
        "atomic_fetch_xor", "bit_test", "bit_set", "bit_clear",
        "bit_test_and_set", "bit_test_and_clear", "refcount_set",
        "refcount_read", "refcount_increment", "refcount_increment_not_zero",
        "refcount_decrement_and_test",
    ]
    if not isinstance(inline, dict) or inline.get("types") != inline_types:
        raise ValueError("core runtime inline type catalog mismatch")
    if inline.get("c_abi") != {
            "implementation": "static-inline-compiler-atomic",
            "read_write_once_widths": [8, 16, 32, 64],
            "atomic_widths": [32, 64],
            "bitmap_bit_order": "least-significant-bit-first",
            "invalid_memory_order": "closure-fault",
            "refcount_fault": "saturate-and-closure-fault",
    }:
        raise ValueError("core runtime inline C ABI mismatch")
    for item in inline_types[:3]:
        name = item["name"]
        if (constants.get(f"sync_{name}_size") != item["size"] or
                constants.get(f"sync_{name}_alignment") != item["alignment"]):
            raise ValueError(f"invalid core runtime inline type: {name}")
    if constants.get("sync_bitmap_word_bits") != 64:
        raise ValueError("invalid core runtime bitmap word")
    if (constants.get("thread_stack_minimum_bytes") != 65536 or
            constants.get("thread_stack_alignment") != 4096 or
            constants.get("thread_priority_highest") != 0 or
            constants.get("thread_priority_default") != 20 or
            constants.get("thread_priority_lowest") != 39):
        raise ValueError("invalid core runtime thread bounds")
    memory_orders = inline.get("memory_orders")
    if ([item.get("name") for item in memory_orders] != memory_order_names or
            [item.get("value") for item in memory_orders] != list(range(1, 6))):
        raise ValueError("core runtime memory order catalog mismatch")
    for value, name in enumerate(memory_order_names, 1):
        if constants.get(f"sync_memory_order_{name}") != value:
            raise ValueError(f"invalid core runtime memory order: {name}")
    inline_operations = inline.get("operations")
    if (not isinstance(inline_operations, list) or
            constants.get("sync_inline_operation_count") !=
            len(inline_operation_names) or
            len(inline_operations) != len(inline_operation_names)):
        raise ValueError("core runtime inline operation count mismatch")
    for operation_id, (operation, name) in enumerate(
            zip(inline_operations, inline_operation_names), 1):
        if (operation.get("id") != operation_id or
                operation.get("name") != name or
                not isinstance(operation.get("inputs"), list) or
                not isinstance(operation.get("outputs"), list) or
                constants.get(f"sync_inline_operation_{name}") != operation_id):
            raise ValueError(f"invalid core runtime inline operation: {name}")

    expected_flags = {
        "memory_flag": ["zero", "atomic", "reclaimable"],
        "cpu_percpu_flag": ["zero"],
        "sync_wait_flag": ["interruptible"],
        "sync_event_flag": ["manual_reset", "initial_signaled"],
        "thread_flag": ["start_parked", "high_priority"],
        "time_wait_flag": ["interruptible"],
        "time_timer_flag": ["deferrable", "pinned"],
        "workqueue_flag": [
            "unbound", "high_priority", "memory_reclaim", "freezable",
            "ordered"],
        "rcu_sync_flag": ["expedited"],
    }
    flags = schema.get("flags")
    if not isinstance(flags, dict) or set(flags) != set(expected_flags):
        raise ValueError("core runtime flag groups mismatch")
    for group, names in expected_flags.items():
        entries = flags[group]
        if [entry.get("name") for entry in entries] != names:
            raise ValueError(f"core runtime flag catalog mismatch: {group}")
        for index, entry in enumerate(entries):
            value = 1 << index
            if (entry.get("value") != value or not entry.get("meaning") or
                    constants.get(f"{group}_{entry['name']}") != value):
                raise ValueError(f"invalid core runtime flag: {group}")

    expected_enumerations = {
        "cpu_context": ["thread", "softirq", "hardirq"],
        "time_clock": ["monotonic", "boottime", "realtime"],
        "time_timer_context": ["atomic", "thread"],
        "rcu_domain": ["classic", "srcu"],
    }
    enumerations = schema.get("enumerations")
    if (not isinstance(enumerations, dict) or
            set(enumerations) != set(expected_enumerations)):
        raise ValueError("core runtime enumeration groups mismatch")
    for group, names in expected_enumerations.items():
        entries = enumerations[group]
        if [entry.get("name") for entry in entries] != names:
            raise ValueError(
                f"core runtime enumeration catalog mismatch: {group}")
        for value, entry in enumerate(entries, 1):
            if (entry.get("value") != value or
                    constants.get(f"{group}_{entry['name']}") != value):
                raise ValueError(f"invalid core runtime enumeration: {group}")
    if schema.get("lifetime") != {
        "binding_scope": "module-node-generation",
        "operation_table_scope": "core-provider-generation",
        "object_owner": "creating-binding",
        "callback_owner": "registering-binding",
        "borrowed_results": [
            "thread.current", "rcu.default_domain", "cpu.percpu_address"],
        "wait_deadline_clock": "monotonic",
        "cpu_topology": "generation-stable",
        "cross_binding_transfer": "explicitly-shared-object-only",
    }:
        raise ValueError("core runtime lifetime contract mismatch")
    if schema.get("lifecycle") != {
        "init_order": [1, 2, 3, 4, 5, 6, 7],
        "quiesce_order": [7, 6, 5, 4, 3, 2, 1],
        "cleanup_order": [7, 6, 5, 4, 3, 2, 1],
        "normal_unbind": "all-owned-objects-released",
        "cleanup_gate": "all-bindings-unbound",
    }:
        raise ValueError("core runtime lifecycle contract mismatch")
    if schema.get("fault") != {
        "unit": "closure-process",
        "entry_failure": "reverse-initialized-rollback",
        "quiesce_failure": "process-termination",
        "cleanup_busy": "process-termination",
        "stale_callback": "closure-fault",
        "ownership_violation": "closure-fault",
        "corruption": "closure-fault",
    }:
        raise ValueError("core runtime fault contract mismatch")
    if schema.get("semantics") != {
        "blocking_may": "sleepable-context-only",
        "blocking_never": "atomic-context-valid",
        "wait_deadline_zero": "infinite-except-time-deadlines",
        "alignment": "power-of-two",
        "reallocate_failure": "original-allocation-preserved",
        "preemption_irq_bh_nesting": "per-thread-balanced",
        "irq_delivery": "logical-cpu-mask-aware",
        "time_clocks":
            "monotonic-running-boottime-suspend-realtime-adjustable",
        "time_deadline": "absolute-selected-clock-zero-or-past-immediate",
        "time_sleep_resolution": "deadline-then-stop-then-interrupt",
        "time_busy_wait": "atomic-context-continuous-execution",
        "timer_period_zero": "one-shot",
        "timer_pinned": "creation-logical-cpu",
        "timer_unpinned": "provider-selected-online-cpu-per-arm",
        "timer_context":
            "atomic-softirq-or-provider-thread-on-assigned-cpu",
        "timer_deferrable":
            "no-independent-cpu-wake-next-provider-activity",
        "timer_arm": "replace-pending-generation-and-queued-delivery",
        "timer_periodic":
            "absolute-phase-preserving-exact-overrun-count",
        "timer_cancel": "remove-pending-and-queued-running-continues",
        "timer_cancel_sync":
            "cancel-then-callback-complete-self-deadlock",
        "timer_callback_execution": "serialized-per-timer",
        "timer_quiesce":
            "cancel-sleep-and-queued-timers-drain-callbacks",
        "workqueue_bound":
            "logical-cpu-domain-cpu-any-captures-submitter",
        "workqueue_unbound": "closure-domain-cpu-any-only",
        "workqueue_maximum_active":
            "per-domain-zero-provider-positive",
        "workqueue_ordered":
            "unbound-explicit-one-no-reclaim-ready-transition-order",
        "workqueue_high_priority": "distinct-priority-zero-pool",
        "workqueue_memory_reclaim":
            "reserved-worker-per-domain-outside-limit",
        "workqueue_freezable": "cancel-pending-on-core-quiesce",
        "work_submission":
            "one-pending-successor-while-idle-or-running",
        "work_delayed": "absolute-monotonic-zero-or-past-immediate",
        "work_reschedule":
            "replace-pending-placement-retain-epochs",
        "work_cancel": "remove-pending-running-continues",
        "work_cancel_sync":
            "cancel-then-callback-complete-self-deadlock",
        "work_flush":
            "work-sequence-or-queue-submission-epoch-snapshot",
        "work_callback_execution":
            "serialized-per-work-native-thread-context",
        "work_quiesce":
            "reject-submit-freezable-cancel-otherwise-drain",
        "rcu_default_domain": "generation-borrowed-classic",
        "rcu_created_domain": "binding-owned-classic-or-srcu",
        "rcu_classic_reader":
            "nonblocking-nestable-preemptible-migratable",
        "rcu_srcu_reader":
            "blocking-nestable-preemptible-migratable",
        "rcu_read_token": "unique-same-execution-thread-domain-once",
        "rcu_read_ordering":
            "lock-acquire-unlock-release-grace-full-boundary",
        "rcu_grace_period":
            "read-sequence-snapshot-later-readers-excluded",
        "rcu_expedited":
            "provider-lowest-latency-same-completion-contract",
        "rcu_call":
            "read-snapshot-domain-fifo-nonblocking-provider-worker",
        "rcu_barrier":
            "callback-sequence-snapshot-later-callbacks-excluded",
        "rcu_quiescent_state":
            "classic-only-outside-domain-read-section",
        "rcu_destroy": "close-admission-drain-readers-and-callbacks",
        "rcu_quiesce": "close-admission-drain-all-stop-worker",
        "spin_preemption": "disabled-while-held",
        "sync_wait_admission": "fifo",
        "sync_wait_resolution":
            "predicate-then-stop-then-interrupt-then-timeout",
        "sync_spurious_wake": "internal-retry",
        "sync_destroy": "caller-serialized-and-busy-while-held-or-waited",
        "sync_quiesce": "cancel-waits-and-reject-new-waits",
        "sync_owner_identity": "process-monotonic-thread-id",
        "thread_state": "starting-running-exited-joined-or-detached",
        "thread_create_gate":
            "attributes-and-entry-gate-established-before-return",
        "thread_join":
            "single-successful-reaper-timeout-preserves-joinability",
        "thread_terminal_serialization":
            "owner-serialized-with-all-handle-operations",
        "thread_detach": "core-reaped-binding-busy-until-entry-return",
        "thread_current": "borrowed-provider-managed-caller",
        "thread_stop": "sticky-cooperative-wake",
        "thread_interrupt": "sticky-interruptible-wait-wake-self-clear",
        "thread_park_resolution":
            "stop-interrupt-unpark-permit-wake-deadline",
        "thread_unpark": "coalesced-single-permit",
        "thread_wake": "current-park-only-nonpersistent",
        "thread_stack": "zero-default-or-aligned-at-least-minimum",
        "thread_cpu_mask":
            "logical-exact-word-count-or-null-all-online",
        "thread_priority":
            "zero-highest-twenty-default-thirty-nine-lowest",
        "thread_exit": "balanced-cpu-state-and-no-owned-locks",
        "thread_quiesce": "reject-create-stop-interrupt-wake-join-all",
        "spin_recursion": "deadlock",
        "mutex_recursion": "deadlock",
        "rwlock_admission": "fifo-reader-phases-with-writer-exclusion",
        "rwlock_recursion_and_upgrade": "deadlock",
        "semaphore_wake": "fifo-direct-grant-before-stored-count",
        "event_auto_reset": "single-consumer-coalesced-signal",
        "event_manual_reset": "latched-broadcast-until-reset",
        "completion_reinit": "busy-while-waited",
        "owned_lock_release": "acquiring-thread-only",
        "refcount_overflow": "saturate-and-fault",
        "refcount_zero": "increment-not-zero-only",
        "completion_model": "counting-with-complete-all-latch",
        "event_model": "manual-or-auto-reset",
        "name_storage": "copied-by-core",
    }:
        raise ValueError("core runtime semantic contract mismatch")


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


def core_abi_input_parameters(name, type_name):
    scalar_types = {
        "boolean": "uint32_t",
        "i32": "int32_t",
        "size": "size_t",
        "u32": "uint32_t",
        "u64": "uint64_t",
    }
    if type_name in scalar_types:
        return [(scalar_types[type_name], name)]
    if type_name == "pointer":
        return [("void *", name)]
    if type_name == "string":
        return [("const char *", name), ("size_t", f"{name}_length")]
    if type_name == "cpu_mask":
        return [
            ("const uint64_t *", f"{name}_words"),
            ("size_t", f"{name}_word_count"),
        ]
    if type_name == "schema_digest":
        return [
            ("const uint8_t", f"{name}[KB2_CORE_RUNTIME_SCHEMA_DIGEST_SIZE]")
        ]
    if type_name == "module_context":
        return [("const struct kobox_module_context *", name)]
    if type_name == "binding":
        return [("struct kb2_core_binding *", name)]
    if type_name.startswith("handle:"):
        handle = type_name.removeprefix("handle:")
        return [(f"kb2_core_{handle}_t", name)]
    if type_name.startswith("callback:"):
        callback = type_name.removeprefix("callback:")
        return [(f"kb2_core_{callback}_fn", name)]
    raise ValueError(f"unsupported core runtime input type: {type_name}")


def core_abi_output_parameters(name, type_name):
    scalar_types = {
        "boolean": "uint32_t *",
        "i32": "int32_t *",
        "size": "size_t *",
        "u32": "uint32_t *",
        "u64": "uint64_t *",
    }
    if type_name in scalar_types:
        return [(scalar_types[type_name], f"{name}_out")]
    if type_name == "pointer":
        return [("void **", f"{name}_out")]
    if type_name == "binding":
        return [("struct kb2_core_binding *", f"{name}_out")]
    if type_name.startswith("handle:"):
        handle = type_name.removeprefix("handle:")
        return [(f"kb2_core_{handle}_t *", f"{name}_out")]
    raise ValueError(f"unsupported core runtime output type: {type_name}")


def core_abi_signature(operation, semantic_types, receiver):
    parameters = []
    if receiver:
        parameters.append(("void *", "binding_object"))
    for name in operation["inputs"]:
        parameters.extend(core_abi_input_parameters(name, semantic_types[name]))
    for name in operation["outputs"]:
        parameters.extend(core_abi_output_parameters(name, semantic_types[name]))
    return parameters


def core_abi_function_field(name, parameters):
    if not parameters:
        return f"\tkb2_core_status_t (*{name})(void);"
    lines = [f"\tkb2_core_status_t (*{name})("]
    for index, (type_name, parameter) in enumerate(parameters):
        comma = "," if index + 1 < len(parameters) else ");"
        separator = "" if type_name.endswith("*") else " "
        lines.append(f"\t\t{type_name}{separator}{parameter}{comma}")
    return "\n".join(lines)


def render_core_inline_primitives():
    lines = [
        "#if !defined(__GNUC__) && !defined(__clang__)",
        '#error "kobox2 core inline primitives require compiler atomics"',
        "#endif",
        "",
        "static inline __attribute__((noreturn)) void",
        "kb2_core_sync_contract_fault(void)",
        "{",
        "\t__builtin_trap();",
        "}",
        "",
        "static inline int kb2_core_sync_load_order(uint32_t order)",
        "{",
        "\tswitch (order) {",
        "\tcase KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELAXED:",
        "\t\treturn __ATOMIC_RELAXED;",
        "\tcase KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQUIRE:",
        "\t\treturn __ATOMIC_ACQUIRE;",
        "\tcase KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_SEQ_CST:",
        "\t\treturn __ATOMIC_SEQ_CST;",
        "\tdefault:",
        "\t\tkb2_core_sync_contract_fault();",
        "\t}",
        "}",
        "",
        "static inline int kb2_core_sync_store_order(uint32_t order)",
        "{",
        "\tswitch (order) {",
        "\tcase KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELAXED:",
        "\t\treturn __ATOMIC_RELAXED;",
        "\tcase KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELEASE:",
        "\t\treturn __ATOMIC_RELEASE;",
        "\tcase KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_SEQ_CST:",
        "\t\treturn __ATOMIC_SEQ_CST;",
        "\tdefault:",
        "\t\tkb2_core_sync_contract_fault();",
        "\t}",
        "}",
        "",
        "static inline int kb2_core_sync_rmw_order(uint32_t order)",
        "{",
        "\tswitch (order) {",
        "\tcase KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELAXED:",
        "\t\treturn __ATOMIC_RELAXED;",
        "\tcase KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQUIRE:",
        "\t\treturn __ATOMIC_ACQUIRE;",
        "\tcase KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELEASE:",
        "\t\treturn __ATOMIC_RELEASE;",
        "\tcase KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQ_REL:",
        "\t\treturn __ATOMIC_ACQ_REL;",
        "\tcase KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_SEQ_CST:",
        "\t\treturn __ATOMIC_SEQ_CST;",
        "\tdefault:",
        "\t\tkb2_core_sync_contract_fault();",
        "\t}",
        "}",
        "",
        "static inline int kb2_core_sync_failure_order(uint32_t order)",
        "{",
        "\treturn kb2_core_sync_load_order(order);",
        "}",
        "",
        "static inline uint32_t kb2_core_sync_compare_orders_valid(",
        "\tuint32_t success_order, uint32_t failure_order)",
        "{",
        "\tswitch (success_order) {",
        "\tcase KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELAXED:",
        "\t\treturn failure_order ==",
        "\t\t       KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELAXED;",
        "\tcase KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQUIRE:",
        "\t\treturn failure_order ==",
        "\t\t\t       KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELAXED ||",
        "\t\t       failure_order ==",
        "\t\t\t       KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQUIRE;",
        "\tcase KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELEASE:",
        "\t\treturn failure_order ==",
        "\t\t       KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELAXED;",
        "\tcase KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQ_REL:",
        "\t\treturn failure_order ==",
        "\t\t\t       KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELAXED ||",
        "\t\t       failure_order ==",
        "\t\t\t       KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQUIRE;",
        "\tcase KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_SEQ_CST:",
        "\t\treturn failure_order ==",
        "\t\t\t       KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELAXED ||",
        "\t\t       failure_order ==",
        "\t\t\t       KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQUIRE ||",
        "\t\t       failure_order ==",
        "\t\t\t       KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_SEQ_CST;",
        "\tdefault:",
        "\t\treturn 0;",
        "\t}",
        "}",
        "",
    ]
    for bits in (8, 16, 32, 64):
        lines.extend([
            f"static inline uint{bits}_t kb2_core_read_once_u{bits}(",
            f"\tconst uint{bits}_t *address)",
            "{",
            "\tif (!address || (uintptr_t)address % sizeof(*address))",
            "\t\tkb2_core_sync_contract_fault();",
            "\treturn __atomic_load_n(address, __ATOMIC_RELAXED);",
            "}",
            "",
            f"static inline void kb2_core_write_once_u{bits}(",
            f"\tuint{bits}_t *address, uint{bits}_t value)",
            "{",
            "\tif (!address || (uintptr_t)address % sizeof(*address))",
            "\t\tkb2_core_sync_contract_fault();",
            "\t__atomic_store_n(address, value, __ATOMIC_RELAXED);",
            "}",
            "",
        ])
    lines.extend([
        "static inline void kb2_core_fence(uint32_t order)",
        "{",
        "\t__atomic_thread_fence(kb2_core_sync_rmw_order(order));",
        "}",
        "",
    ])
    atomic_operations = (
        ("fetch_add", "__atomic_fetch_add"),
        ("fetch_sub", "__atomic_fetch_sub"),
        ("fetch_and", "__atomic_fetch_and"),
        ("fetch_or", "__atomic_fetch_or"),
        ("fetch_xor", "__atomic_fetch_xor"),
    )
    for bits in (32, 64):
        type_name = f"kb2_core_atomic{bits}_t"
        value_type = f"uint{bits}_t"
        prefix = f"kb2_core_atomic{bits}"
        lines.extend([
            f"static inline {value_type} {prefix}_load(",
            f"\tconst {type_name} *atomic, uint32_t order)",
            "{",
            "\tif (!atomic || (uintptr_t)atomic % sizeof(*atomic))",
            "\t\tkb2_core_sync_contract_fault();",
            "\treturn __atomic_load_n(&atomic->value,",
            "\t\t\t       kb2_core_sync_load_order(order));",
            "}",
            "",
            f"static inline void {prefix}_store(",
            f"\t{type_name} *atomic, {value_type} value, uint32_t order)",
            "{",
            "\tif (!atomic || (uintptr_t)atomic % sizeof(*atomic))",
            "\t\tkb2_core_sync_contract_fault();",
            "\t__atomic_store_n(&atomic->value, value,",
            "\t\t\t kb2_core_sync_store_order(order));",
            "}",
            "",
            f"static inline {value_type} {prefix}_exchange(",
            f"\t{type_name} *atomic, {value_type} value, uint32_t order)",
            "{",
            "\tif (!atomic || (uintptr_t)atomic % sizeof(*atomic))",
            "\t\tkb2_core_sync_contract_fault();",
            "\treturn __atomic_exchange_n(&atomic->value, value,",
            "\t\t\t\t kb2_core_sync_rmw_order(order));",
            "}",
            "",
            f"static inline uint32_t {prefix}_compare_exchange(",
            f"\t{type_name} *atomic, {value_type} *expected,",
            f"\t{value_type} desired, uint32_t success_order,",
            "\tuint32_t failure_order)",
            "{",
            "\tint success_builtin;",
            "\tint failure_builtin;",
            "",
            "\tif (!atomic || (uintptr_t)atomic % sizeof(*atomic) ||",
            "\t    !expected || (uintptr_t)expected % sizeof(*expected) ||",
            "\t    !kb2_core_sync_compare_orders_valid(success_order,",
            "\t\t\t\t\t       failure_order))",
            "\t\tkb2_core_sync_contract_fault();",
            "\tsuccess_builtin = kb2_core_sync_rmw_order(success_order);",
            "\tfailure_builtin = kb2_core_sync_failure_order(failure_order);",
            "\treturn __atomic_compare_exchange_n(",
            "\t\t&atomic->value, expected, desired, 0, success_builtin,",
            "\t\tfailure_builtin);",
            "}",
            "",
        ])
        for name, builtin in atomic_operations:
            lines.extend([
                f"static inline {value_type} {prefix}_{name}(",
                f"\t{type_name} *atomic, {value_type} value, uint32_t order)",
                "{",
                "\tif (!atomic || (uintptr_t)atomic % sizeof(*atomic))",
                "\t\tkb2_core_sync_contract_fault();",
                f"\treturn {builtin}(&atomic->value, value,",
                "\t\t\t       kb2_core_sync_rmw_order(order));",
                "}",
                "",
            ])
    lines.extend([
        "static inline uint32_t kb2_core_bit_test(",
        "\tconst kb2_core_bitmap_word_t *bitmap, uint32_t bit,",
        "\tuint32_t order)",
        "{",
        "\tif (!bitmap || (uintptr_t)bitmap % sizeof(*bitmap) ||",
        "\t    bit >= KB2_CORE_RUNTIME_SYNC_BITMAP_WORD_BITS)",
        "\t\tkb2_core_sync_contract_fault();",
        "\treturn !!(__atomic_load_n(bitmap,",
        "\t\t\t\t kb2_core_sync_load_order(order)) &",
        "\t\t  (UINT64_C(1) << bit));",
        "}",
        "",
        "static inline void kb2_core_bit_set(",
        "\tkb2_core_bitmap_word_t *bitmap, uint32_t bit, uint32_t order)",
        "{",
        "\tif (!bitmap || (uintptr_t)bitmap % sizeof(*bitmap) ||",
        "\t    bit >= KB2_CORE_RUNTIME_SYNC_BITMAP_WORD_BITS)",
        "\t\tkb2_core_sync_contract_fault();",
        "\t(void)__atomic_fetch_or(bitmap, UINT64_C(1) << bit,",
        "\t\t\t\tkb2_core_sync_rmw_order(order));",
        "}",
        "",
        "static inline void kb2_core_bit_clear(",
        "\tkb2_core_bitmap_word_t *bitmap, uint32_t bit, uint32_t order)",
        "{",
        "\tif (!bitmap || (uintptr_t)bitmap % sizeof(*bitmap) ||",
        "\t    bit >= KB2_CORE_RUNTIME_SYNC_BITMAP_WORD_BITS)",
        "\t\tkb2_core_sync_contract_fault();",
        "\t(void)__atomic_fetch_and(bitmap, ~(UINT64_C(1) << bit),",
        "\t\t\t\t kb2_core_sync_rmw_order(order));",
        "}",
        "",
        "static inline uint32_t kb2_core_bit_test_and_set(",
        "\tkb2_core_bitmap_word_t *bitmap, uint32_t bit, uint32_t order)",
        "{",
        "\tuint64_t previous;",
        "",
        "\tif (!bitmap || (uintptr_t)bitmap % sizeof(*bitmap) ||",
        "\t    bit >= KB2_CORE_RUNTIME_SYNC_BITMAP_WORD_BITS)",
        "\t\tkb2_core_sync_contract_fault();",
        "\tprevious = __atomic_fetch_or(bitmap, UINT64_C(1) << bit,",
        "\t\t\t\t     kb2_core_sync_rmw_order(order));",
        "\treturn !!(previous & (UINT64_C(1) << bit));",
        "}",
        "",
        "static inline uint32_t kb2_core_bit_test_and_clear(",
        "\tkb2_core_bitmap_word_t *bitmap, uint32_t bit, uint32_t order)",
        "{",
        "\tuint64_t previous;",
        "",
        "\tif (!bitmap || (uintptr_t)bitmap % sizeof(*bitmap) ||",
        "\t    bit >= KB2_CORE_RUNTIME_SYNC_BITMAP_WORD_BITS)",
        "\t\tkb2_core_sync_contract_fault();",
        "\tprevious = __atomic_fetch_and(bitmap, ~(UINT64_C(1) << bit),",
        "\t\t\t\t      kb2_core_sync_rmw_order(order));",
        "\treturn !!(previous & (UINT64_C(1) << bit));",
        "}",
        "",
        "static inline void kb2_core_refcount_set(",
        "\tkb2_core_refcount_t *refcount, uint32_t value)",
        "{",
        "\tif (!refcount || (uintptr_t)refcount % sizeof(*refcount) ||",
        "\t    value == UINT32_MAX)",
        "\t\tkb2_core_sync_contract_fault();",
        "\t__atomic_store_n(&refcount->value, value, __ATOMIC_RELAXED);",
        "}",
        "",
        "static inline uint32_t kb2_core_refcount_read(",
        "\tconst kb2_core_refcount_t *refcount)",
        "{",
        "\tif (!refcount || (uintptr_t)refcount % sizeof(*refcount))",
        "\t\tkb2_core_sync_contract_fault();",
        "\treturn __atomic_load_n(&refcount->value, __ATOMIC_RELAXED);",
        "}",
        "",
        "static inline void kb2_core_refcount_increment(",
        "\tkb2_core_refcount_t *refcount)",
        "{",
        "\tuint32_t observed;",
        "",
        "\tif (!refcount || (uintptr_t)refcount % sizeof(*refcount))",
        "\t\tkb2_core_sync_contract_fault();",
        "\tobserved = __atomic_load_n(&refcount->value, __ATOMIC_RELAXED);",
        "\tfor (;;) {",
        "\t\tif (observed == UINT32_MAX)",
        "\t\t\tkb2_core_sync_contract_fault();",
        "\t\tif (!observed || observed == UINT32_MAX - 1) {",
        "\t\t\tif (__atomic_compare_exchange_n(",
        "\t\t\t\t    &refcount->value, &observed, UINT32_MAX, 0,",
        "\t\t\t\t    __ATOMIC_RELAXED, __ATOMIC_RELAXED))",
        "\t\t\t\tkb2_core_sync_contract_fault();",
        "\t\t\tcontinue;",
        "\t\t}",
        "\t\tif (__atomic_compare_exchange_n(",
        "\t\t\t    &refcount->value, &observed, observed + 1, 0,",
        "\t\t\t    __ATOMIC_RELAXED, __ATOMIC_RELAXED))",
        "\t\t\treturn;",
        "\t}",
        "}",
        "",
        "static inline uint32_t kb2_core_refcount_increment_not_zero(",
        "\tkb2_core_refcount_t *refcount)",
        "{",
        "\tuint32_t observed;",
        "",
        "\tif (!refcount || (uintptr_t)refcount % sizeof(*refcount))",
        "\t\tkb2_core_sync_contract_fault();",
        "\tobserved = __atomic_load_n(&refcount->value, __ATOMIC_RELAXED);",
        "\tfor (;;) {",
        "\t\tif (!observed)",
        "\t\t\treturn 0;",
        "\t\tif (observed == UINT32_MAX)",
        "\t\t\tkb2_core_sync_contract_fault();",
        "\t\tif (observed == UINT32_MAX - 1) {",
        "\t\t\tif (__atomic_compare_exchange_n(",
        "\t\t\t\t    &refcount->value, &observed, UINT32_MAX, 0,",
        "\t\t\t\t    __ATOMIC_RELAXED, __ATOMIC_RELAXED))",
        "\t\t\t\tkb2_core_sync_contract_fault();",
        "\t\t\tcontinue;",
        "\t\t}",
        "\t\tif (__atomic_compare_exchange_n(",
        "\t\t\t    &refcount->value, &observed, observed + 1, 0,",
        "\t\t\t    __ATOMIC_RELAXED, __ATOMIC_RELAXED))",
        "\t\t\treturn 1;",
        "\t}",
        "}",
        "",
        "static inline uint32_t kb2_core_refcount_decrement_and_test(",
        "\tkb2_core_refcount_t *refcount)",
        "{",
        "\tuint32_t observed;",
        "",
        "\tif (!refcount || (uintptr_t)refcount % sizeof(*refcount))",
        "\t\tkb2_core_sync_contract_fault();",
        "\tobserved = __atomic_load_n(&refcount->value, __ATOMIC_RELAXED);",
        "\tfor (;;) {",
        "\t\tif (!observed) {",
        "\t\t\tif (__atomic_compare_exchange_n(",
        "\t\t\t\t    &refcount->value, &observed, UINT32_MAX, 0,",
        "\t\t\t\t    __ATOMIC_RELAXED, __ATOMIC_RELAXED))",
        "\t\t\t\tkb2_core_sync_contract_fault();",
        "\t\t\tcontinue;",
        "\t\t}",
        "\t\tif (observed == UINT32_MAX)",
        "\t\t\tkb2_core_sync_contract_fault();",
        "\t\tif (__atomic_compare_exchange_n(",
        "\t\t\t    &refcount->value, &observed, observed - 1, 0,",
        "\t\t\t    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))",
        "\t\t\treturn observed == 1;",
        "\t}",
        "}",
        "",
    ])
    return lines


def render_core_runtime_abi(schema):
    constants = schema["constants"]
    semantic_types = schema["c_abi"]["semantic_types"]
    handles = sorted({
        type_name.removeprefix("handle:")
        for type_name in semantic_types.values()
        if type_name.startswith("handle:")
    })
    lines = [
        "/* SPDX-License-Identifier: MIT */",
        "/* Generated by tools/generate_protocol.py. Do not edit. */",
        "",
        "#ifndef KOBOX2_CORE_RUNTIME_H",
        "#define KOBOX2_CORE_RUNTIME_H",
        "",
        "#include <kobox2/core_runtime_layout.h>",
        "",
        "#ifdef __KERNEL__",
        "#include <linux/types.h>",
        "typedef u8 uint8_t;",
        "typedef u32 uint32_t;",
        "typedef u64 uint64_t;",
        "typedef s32 int32_t;",
        "#ifndef UINT32_C",
        "#define UINT32_C(value) value##U",
        "#endif",
        "#ifndef UINT32_MAX",
        "#define UINT32_MAX (~(uint32_t)0)",
        "#endif",
        "#ifndef UINT64_C",
        "#define UINT64_C(value) value##ULL",
        "#endif",
        "#else",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "#endif",
        "",
        "struct kobox_module_context;",
        "",
        "typedef int32_t kb2_core_status_t;",
        "",
    ]
    for interface in schema["interfaces"]:
        mask = sum(1 << (item - 1) for item in interface["dependencies"])
        lines.append(
            f"#define KB2_CORE_RUNTIME_INTERFACE_"
            f"{identifier(interface['name'])}_DEPENDENCY_MASK "
            f"UINT32_C(0x{mask:08x})"
        )
    lines.append("")
    for handle in handles:
        lines.extend([
            f"struct kb2_core_{handle};",
            (f"typedef struct kb2_core_{handle} *"
             f"kb2_core_{handle}_t;"),
        ])
    lines.extend([
        "",
        "typedef kb2_core_status_t (*kb2_core_cache_constructor_fn)(",
        "\tvoid *allocation, size_t object_size, void *argument);",
        "typedef void (*kb2_core_cache_destructor_fn)(",
        "\tvoid *allocation, size_t object_size, void *argument);",
        "typedef int32_t (*kb2_core_thread_entry_fn)(void *argument);",
        "typedef void (*kb2_core_timer_callback_fn)(",
        "\tkb2_core_time_timer_t timer, void *argument,",
        "\tuint64_t expiration_count);",
        "typedef void (*kb2_core_work_callback_fn)(",
        "\tkb2_core_workqueue_work_t work, void *argument);",
        "typedef void (*kb2_core_rcu_callback_fn)(void *argument);",
        "",
        "typedef struct kb2_core_atomic32 {",
        "\tuint32_t value;",
        "} kb2_core_atomic32_t;",
        "",
        "typedef struct kb2_core_atomic64 {",
        "\tuint64_t value;",
        "} kb2_core_atomic64_t;",
        "",
        "typedef struct kb2_core_refcount {",
        "\tuint32_t value;",
        "} kb2_core_refcount_t;",
        "",
        "typedef uint64_t kb2_core_bitmap_word_t;",
        "",
        *render_core_inline_primitives(),
        "struct kb2_core_binding {",
        "\tconst void *operations;",
        "\tvoid *object;",
        "};",
        "",
        "struct kb2_core_interface_header {",
        "\tuint32_t size;",
        "\tuint8_t identity[KB2_CORE_RUNTIME_ABI_IDENTITY_SIZE];",
        "\tuint32_t interface_id;",
        "\tuint32_t reserved;",
        "\tuint8_t schema_digest[KB2_CORE_RUNTIME_SCHEMA_DIGEST_SIZE];",
        "};",
        "",
        "struct kb2_core_directory {",
        "\tuint32_t size;",
        "\tuint8_t identity[KB2_CORE_RUNTIME_ABI_IDENTITY_SIZE];",
        "\tuint8_t schema_digest[KB2_CORE_RUNTIME_SCHEMA_DIGEST_SIZE];",
    ])
    for operation in schema["directory_operations"]:
        lines.append(core_abi_function_field(
            operation["name"],
            core_abi_signature(operation, semantic_types, False),
        ))
    lines.extend(["};", ""])
    for interface in schema["interfaces"]:
        lines.extend([
            f"struct kb2_core_{interface['name']}_operations {{",
            "\tstruct kb2_core_interface_header header;",
        ])
        for operation in interface["operations"]:
            lines.append(core_abi_function_field(
                operation["name"],
                core_abi_signature(operation, semantic_types, True),
            ))
        lines.extend(["};", ""])

    lines.extend([
        "_Static_assert(sizeof(void *) == 8, \"core runtime requires LP64 pointers\");",
        "_Static_assert(sizeof(size_t) == 8, \"core runtime requires LP64 size_t\");",
        "_Static_assert(sizeof(struct kb2_core_binding) ==",
        "\t       KB2_CORE_RUNTIME_BINDING_SIZE, \"binding ABI size\");",
        "_Static_assert(offsetof(struct kb2_core_binding, operations) == 0,",
        "\t       \"binding operations ABI offset\");",
        "_Static_assert(offsetof(struct kb2_core_binding, object) == 8,",
        "\t       \"binding object ABI offset\");",
        "_Static_assert(sizeof(struct kb2_core_interface_header) ==",
        "\t       KB2_CORE_RUNTIME_INTERFACE_HEADER_SIZE,",
        "\t       \"interface header ABI size\");",
        "_Static_assert(offsetof(struct kb2_core_interface_header, size) == 0,",
        "\t       \"interface size ABI offset\");",
        "_Static_assert(offsetof(struct kb2_core_interface_header, identity) == 4,",
        "\t       \"interface identity ABI offset\");",
        "_Static_assert(offsetof(struct kb2_core_interface_header, interface_id) == 8,",
        "\t       \"interface ID ABI offset\");",
        "_Static_assert(offsetof(struct kb2_core_interface_header, reserved) == 12,",
        "\t       \"interface reserved ABI offset\");",
        "_Static_assert(offsetof(struct kb2_core_interface_header, schema_digest) == 16,",
        "\t       \"interface digest ABI offset\");",
        "_Static_assert(sizeof(struct kb2_core_directory) ==",
        "\t       KB2_CORE_RUNTIME_DIRECTORY_SIZE, \"directory ABI size\");",
        "_Static_assert(offsetof(struct kb2_core_directory, bind) == 40,",
        "\t       \"directory bind ABI offset\");",
        "_Static_assert(offsetof(struct kb2_core_directory, unbind) == 48,",
        "\t       \"directory unbind ABI offset\");",
        "_Static_assert(sizeof(kb2_core_atomic32_t) ==",
        "\t       KB2_CORE_RUNTIME_SYNC_ATOMIC32_SIZE, \"atomic32 ABI size\");",
        "_Static_assert(_Alignof(kb2_core_atomic32_t) ==",
        "\t       KB2_CORE_RUNTIME_SYNC_ATOMIC32_ALIGNMENT,",
        "\t       \"atomic32 ABI alignment\");",
        "_Static_assert(sizeof(kb2_core_atomic64_t) ==",
        "\t       KB2_CORE_RUNTIME_SYNC_ATOMIC64_SIZE, \"atomic64 ABI size\");",
        "_Static_assert(_Alignof(kb2_core_atomic64_t) ==",
        "\t       KB2_CORE_RUNTIME_SYNC_ATOMIC64_ALIGNMENT,",
        "\t       \"atomic64 ABI alignment\");",
        "_Static_assert(sizeof(kb2_core_refcount_t) ==",
        "\t       KB2_CORE_RUNTIME_SYNC_REFCOUNT_SIZE, \"refcount ABI size\");",
        "_Static_assert(_Alignof(kb2_core_refcount_t) ==",
        "\t       KB2_CORE_RUNTIME_SYNC_REFCOUNT_ALIGNMENT,",
        "\t       \"refcount ABI alignment\");",
    ])
    for interface in schema["interfaces"]:
        structure = f"struct kb2_core_{interface['name']}_operations"
        macro = f"KB2_CORE_RUNTIME_{identifier(interface['name'])}_TABLE_SIZE"
        lines.extend([
            f"_Static_assert(sizeof({structure}) ==",
            f"\t       {macro},",
            f"\t       \"{interface['name']} table ABI size\");",
        ])
        for operation in interface["operations"]:
            offset = constants["interface_header_size"] + 8 * (operation["id"] - 1)
            lines.extend([
                f"_Static_assert(offsetof({structure}, {operation['name']}) == {offset},",
                (f"\t       \"{interface['name']}.{operation['name']} "
                 "ABI offset\");"),
            ])
    lines.extend(["", "#endif", ""])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()

    try:
        loaded = [(load_schema(schema), output, prefix, guard)
                  for schema, output, prefix, guard in SCHEMAS]
        schemas = {schema["name"]: schema for schema, _, _, _ in loaded}
        validate_memory_arena(schemas["kobox2.memory-arena"])
        validate_device_resource(
            schemas["kobox2.pci-function"], "kobox2.pci-function", "device",
            ((0, "device"),), 7,
            ("identity", "config_read", "config_write", "bar_info",
             "bar_map", "bar_unmap", "bar_read", "bar_write"),
        )
        validate_device_resource(
            schemas["kobox2.dma-domain"], "kobox2.dma-domain", "device",
            ((0, "domain"), (1, "context")), 5,
            ("constraints", "allocation_create", "allocation_release",
             "mapping_create", "mapping_release", "sync_for_cpu",
             "sync_for_device", "drain", "mapping_create_span"),
        )
        validate_device_resource(
            schemas["kobox2.irq-endpoint"], "kobox2.irq-endpoint",
            "notification", ((0, "endpoint"),), 1,
            ("identity", "handler_register", "handler_unregister", "enable",
             "disable_and_synchronize"),
        )
        validate_core_runtime(schemas["kobox2.core-runtime"])
        validate_gpu_family(schemas)
        rendered = [(output, render(schema, prefix, guard))
                    for schema, output, prefix, guard in loaded]
        rendered.append((
            CORE_RUNTIME_ABI_HEADER,
            render_core_runtime_abi(schemas["kobox2.core-runtime"]),
        ))
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
