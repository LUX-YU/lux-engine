"""Aggregate Render's annotation projection into its export and tool projections."""
import argparse
import json
from pathlib import Path
from plugin_descriptor import publish

parser = argparse.ArgumentParser()
parser.add_argument("--records", nargs="+", required=True)
parser.add_argument("--output", required=True)
parser.add_argument("--exports", required=True)
args = parser.parse_args()
records = [record for path in args.records for record in json.loads(Path(path).read_text(encoding="utf-8"))]
identities = set()
for record in records:
    feature = record["feature"]
    if not feature["id"] or feature["id"] in identities or feature["operation_count"] > 16:
        raise ValueError("invalid or duplicate RenderFeature declaration: " + feature["id"])
    identities.add(feature["id"])
    for requirement in record["requires"].split(","):
        requirement = requirement.strip()
        if requirement:
            optional = requirement.endswith("?")
            feature["dependencies"].append(dict(feature=requirement.rstrip("?"), version=1, optional=optional))
value = dict(format="lux.engine.plugin", version=1,
    plugin=dict(id="lux.builtin.render", version=1, source="builtin", author="Lux",
        description="Builtin render feature implementations.", runtime_library=dict(exports=["render"]), dependencies=[]),
    abilities=[], implementations=[], systems=[], components=[],
    configurations=[r["configuration"] for r in records], render_features=[r["feature"] for r in records],
    render_scene_bindings=[])

code = "// Generated from LUX_COMM_CONFIG / LUX_COMM_VARIANT.\n#include <lux/engine/function/render/features/BuiltinFeatures.hpp>\n"
code += "#include <lux/engine/function/render/client/RenderPluginExports.hpp>\n#include <array>\n"
for header in dict.fromkeys(r["header"] for r in records): code += f"#include <{header}>\n"
code += "namespace lux::render {\nstd::span<const RenderFeatureRegistration> builtinRenderFeatureRegistrations() noexcept\n{\n    static const std::array entries{\n"
code += ",\n".join("        " + r["registration"] for r in records)
code += "\n    };\n    return entries;\n}\n}\n"
code += r'''extern "C" LUX_ENGINE_FUNCTION_RENDER_FEATURES_PUBLIC
const lux::render::RenderPluginExports *lux_render_exports_v1() noexcept
{
    static const auto entries = lux::render::builtinRenderFeatureRegistrations();
    static const lux::render::RenderPluginExports exports{
        sizeof(lux::render::RenderPluginExports), 1, entries.data(), static_cast<std::uint32_t>(entries.size())
    };
    return &exports;
}
'''
publish([(args.output, json.dumps(value, indent=2, ensure_ascii=False) + "\n"), (args.exports, code)])
