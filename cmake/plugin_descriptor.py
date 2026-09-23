"""Produce the DLL identity and tool description from the same immutable input."""
import argparse
import hashlib
import json
from pathlib import Path


def publish(outputs):
    # Prepare every output before replacing any destination. Build dependents run
    # only after this command succeeds; a failed replacement restores the old set.
    import os
    import tempfile
    prepared = []
    replaced = []
    try:
        for path, data in outputs:
            path = Path(path)
            encoded = data.encode("utf-8")
            previous = path.read_bytes() if path.exists() else None
            if previous == encoded:
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            fd, name = tempfile.mkstemp(dir=path.parent, prefix=path.name + ".")
            with os.fdopen(fd, "wb") as stream:
                stream.write(encoded)
                stream.flush()
                os.fsync(stream.fileno())
            prepared.append((path, Path(name), previous))
        for path, temporary, previous in prepared:
            os.replace(temporary, path)
            replaced.append((path, previous))
    except BaseException:
        for path, previous in reversed(replaced):
            if previous is None:
                path.unlink()
            else:
                path.write_bytes(previous)
        raise
    finally:
        for _, temporary, _ in prepared:
            temporary.unlink(missing_ok=True)


def write_if_changed(path, data):
    publish([(path, data)])


def main():
    parser = argparse.ArgumentParser()
    for name in ("input", "identity", "output", "library", "sdk-abi"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--editor-library")
    parser.add_argument("--value-fragments", nargs="*", default=[])
    parser.add_argument("--sources", nargs="+", required=True)
    args = parser.parse_args()
    value = json.loads(Path(args.input).read_text(encoding="utf-8"))
    declarations = [v for path in args.value_fragments for v in json.loads(Path(path).read_text(encoding="utf-8"))]
    by_schema = {v["schema"]: v for v in declarations if v["schema"]}
    by_type = {v["cpp_type"]: v for v in declarations}
    for kind in ("components", "configurations"):
        for record in value[kind]:
            cpp_type = record.pop("_cpp_type", "")
            generated = by_schema.get(record["id"]) or by_type.get(cpp_type)
            if generated:
                old = {f["id"]: f for f in record["fields"]}
                record["fields"] = [dict(old.get(f["id"], {}), **f) for f in generated["fields"]]
    if not args.editor_library:
        value["plugin"].pop("editor_library", None)
    # Only runtime contracts enter the declaration fingerprint. Presentation and
    # installation locations do not change a module's executable contract.
    projection = {key: value[key] for key in ("abilities", "implementations", "systems", "components", "configurations", "render_features", "render_scene_bindings")}
    projection["module"] = {key: value["plugin"][key] for key in ("id", "version", "dependencies")}
    def runtime_fields(item):
        if isinstance(item, dict):
            return {k: runtime_fields(v) for k, v in item.items() if k not in {
                "display_name", "description", "unit", "default_display", "minimum", "maximum", "read_only"}}
        if isinstance(item, list): return [runtime_fields(v) for v in item]
        return item
    declaration = json.dumps(runtime_fields(projection), sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
    digest = hashlib.sha256(declaration).hexdigest()
    build = hashlib.sha256(declaration + args.sdk_abi.encode())
    # Content, not absolute source paths, defines a reproducible build identity.
    for source in args.sources:
        data = Path(source).read_bytes()
        build.update(len(data).to_bytes(8, "little"))
        build.update(data)
    build_id = build.hexdigest()
    plugin = value["plugin"]
    library = plugin["runtime_library"]
    library.update(path=args.library, interface_version=1, sdk_abi=args.sdk_abi,
                   build_id=build_id, declaration_digest=digest)
    if args.editor_library:
        plugin["editor_library"] = dict(path=args.editor_library, interface_version=1, sdk_abi=args.sdk_abi,
            build_id=build_id, declaration_digest=digest, exports=["editor"])
    quote = lambda text: json.dumps(text, ensure_ascii=True)
    code = f'''// Generated together with the installed plugin description.
#include <lux/engine/dynamic_library/LibraryExport.hpp>
#if defined(_WIN32)
#define LUX_PLUGIN_EXPORT __declspec(dllexport)
#else
#define LUX_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
extern "C" LUX_PLUGIN_EXPORT const lux::engine::platform::LibraryExportIdentity *lux_plugin_identity_v1() noexcept
{{
    static const lux::engine::platform::LibraryExportIdentity identity{{
        sizeof(lux::engine::platform::LibraryExportIdentity), 1,
        {quote(plugin['id'])}, {plugin['version']}, {quote(args.sdk_abi)},
        {quote(build_id)}, {quote(digest)}
    }};
    return &identity;
}}
'''
    publish([(args.identity, code), (args.output, json.dumps(value, indent=2, ensure_ascii=False) + "\n")])


if __name__ == "__main__":
    main()
