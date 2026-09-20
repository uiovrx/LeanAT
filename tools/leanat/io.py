import hashlib
import json
import os
import tempfile
from pathlib import Path


class ToolError(ValueError):
    def __init__(self, code, message, exit_code=2):
        super().__init__(message)
        self.code, self.exit_code = code, exit_code


def digest(data):
    return hashlib.sha256(data).hexdigest()


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ToolError("DuplicateKey", key)
        result[key] = value
    return result


def read_json(path, max_bytes=8_388_608, max_depth=64):
    with open(path, "rb") as stream:
        raw = stream.read(max_bytes + 1)
    if len(raw) > max_bytes:
        raise ToolError("ReadLimit", "JSON exceeds host byte limit")
    # Check nesting before recursive JSON decoding; brackets inside strings do not count.
    depth = 0
    quoted = escaped = False
    for byte in raw:
        if quoted:
            if escaped: escaped = False
            elif byte == 92: escaped = True
            elif byte == 34: quoted = False
        elif byte == 34: quoted = True
        elif byte in (91, 123):
            depth += 1
            if depth > max_depth: raise ToolError("ReadLimit", "JSON nesting exceeds host depth limit")
        elif byte in (93, 125): depth -= 1
    try:
        return json.loads(raw, object_pairs_hook=unique_pairs,
                          parse_constant=lambda x: (_ for _ in ()).throw(ToolError("InvalidNumber", x)))
    except (json.JSONDecodeError, UnicodeDecodeError, RecursionError) as exc:
        raise ToolError("InvalidJSON", str(exc)) from exc


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp = tempfile.mkstemp(prefix=".leanat-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(json.dumps(value, ensure_ascii=False, indent=2).encode("utf-8") + b"\n")
        os.replace(temp, path)
    finally:
        if os.path.exists(temp): os.unlink(temp)


def u64(value, field):
    if not isinstance(value, str) or not value.isascii() or not value.isdecimal() or (len(value)>1 and value[0]=="0") or int(value)>2**64-1:
        raise ToolError("TraceSchemaMismatch", field + " must be a canonical UInt64 decimal string")
    return int(value)


def require(obj, keys, context):
    if not isinstance(obj, dict): raise ToolError("SchemaMismatch", context + " must be an object")
    missing = set(keys) - obj.keys()
    if missing: raise ToolError("SchemaMismatch", context + " missing " + ", ".join(sorted(missing)))
