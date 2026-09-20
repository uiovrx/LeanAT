"""Strict shared helpers; raw observations remain in the enclosing evidence."""
import copy


def identity(value):
    if isinstance(value, dict) and value.get("kind") == "handle":
        value = value["identity"]
    if isinstance(value, dict) and set(value) == {"handle"}:
        value = value["handle"]
    required = {"kind", "domain", "store", "slot", "generation", "owner"}
    if not isinstance(value, dict) or set(value) != required:
        raise ValueError("complete identity required")
    result = {key: str(value[key]) for key in required}
    for key, number in result.items():
        if not number.isascii() or not number.isdecimal() or str(int(number)) != number:
            raise ValueError("canonical identity integer required: " + key)
    return result


class IdentityBijection:
    def __init__(self, entries):
        self.maps = {side: {} for side in ("model", "exec", "native")}
        self.entries = copy.deepcopy(entries)
        roles = set()
        for entry in entries:
            role = entry["role"]
            if not isinstance(role, str) or not role or role in roles:
                raise ValueError("duplicate or missing semantic identity role")
            roles.add(role)
            parsed = {side: identity(entry[side]) for side in self.maps}
            for field in ("kind", "domain", "owner"):
                if len({item[field] for item in parsed.values()}) != 1:
                    raise ValueError("identity authority differs: " + role + "." + field)
            for side, item in parsed.items():
                key = tuple(sorted(item.items()))
                if key in self.maps[side]:
                    raise ValueError("identity mapping is not bijective: " + side)
                self.maps[side][key] = role

    def normalize(self, value, side):
        if isinstance(value, dict):
            if value.get("kind") == "handle" or set(value) == {"handle"}:
                parsed = identity(value)
                role = self.maps[side].get(tuple(sorted(parsed.items())))
                if role is None:
                    # Unmapped identities stay complete. Never erase a stale generation.
                    return {"kind": "handle", "identity": parsed}
                return {"semanticIdentity": role, "kind": parsed["kind"],
                        "domain": parsed["domain"], "owner": parsed["owner"]}
            return {key: self.normalize(item, side) for key, item in value.items()}
        if isinstance(value, list):
            return [self.normalize(item, side) for item in value]
        return value


def observed_value(value):
    """Convert native writer forms to an untyped semantic tree, preserving all data."""
    if not isinstance(value, dict):
        return value
    if set(value) == {"u64"}:
        return {"integer": str(value["u64"])}
    if set(value) == {"array"}:
        return [observed_value(item) for item in value["array"]]
    if set(value) == {"bytes"}:
        return {"bytes": [int(item) for item in value["bytes"]]}
    if set(value) == {"handle"}:
        return {"kind": "handle", "identity": identity(value)}
    if value.get("kind") == "bits":
        return {"integer": value["value"]}
    if value.get("kind") == "record":
        return [observed_value(item) for item in value["fields"]]
    if value.get("kind") == "vec":
        return [observed_value(item) for item in value["values"]]
    if value.get("kind") == "bytes":
        return {"bytes": value["data"]}
    if value.get("kind") == "handle":
        return {"kind": "handle", "identity": identity(value)}
    return {key: observed_value(item) for key, item in value.items()}
