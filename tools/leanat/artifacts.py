from pathlib import Path
from .io import ToolError, read_json, digest, canonical
from .evidence import validate_manifest
from .trace import lookup_source


def validate_emitted_package(directory):
    directory = Path(directory).resolve()
    validated = validate_manifest(read_json(directory / "manifest.json"), directory)
    descriptor = (directory / "model.execir.bin").read_bytes()
    source_map = read_json(directory / "source-map.json")
    entries = source_map.get("entries")
    if not isinstance(entries, list) or not entries or len(entries) > 65536:
        raise ToolError("MissingSourceMap", "nonempty bounded emitted source map required")
    for entry in entries:
        lookup_source(source_map, descriptor, entry["location"], directory)
    files = {p.relative_to(directory).as_posix(): digest(p.read_bytes()) for p in sorted(directory.rglob("*")) if p.is_file()}
    return {"descriptorHash": digest(descriptor), "sourceMapHash": digest((directory / "source-map.json").read_bytes()),
            "packageHash": digest(canonical(files)), "sourceMapEntriesValidated": len(entries),
            "artifactHashes": validated["hashes"], "packageFiles": files}
