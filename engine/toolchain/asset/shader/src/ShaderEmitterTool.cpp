// lux_shader_emitter — a thin CLI shell wrapping .lglsl → canonical GLSL.
//
// All the logic lives in the Toolchain-owned lux::shadergen::lglsl library;
// this tool only does argv parsing and
// file IO. It ships with the SDK (install/export), and external projects'
// builds invoke it via find_program to compile their own .lglsl files — the
// same host-tool pattern as lux_meta_generator / lux_asset_packer.
//
// Usage:
//   lux_shader_emitter --in <file.lglsl> [--out <file>] [--print-meta] [--header]
//     --in          input .lglsl / .lglslh source
//     --out         output the full GLSL (omitted = validate only, dry run)
//     --print-meta  print pragma metadata to stdout, one item per line
//                   (stage=… / entry=… / variant=…), consumed by the build
//                   manifest (cmake)
//     --header      shared-header mode (.lglslh: declaration injection only,
//                   does not require lux-shader)
//
// Exit codes: 0 = success; 1 = emission failed (error includes line number,
// printed to stderr); 2 = usage error.
#include <lux/engine/toolchain/shader/lglsl/LglslEmitter.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
    [[nodiscard]] const char* toString(lux::rdesc::EShaderType stage) noexcept
    {
        switch (stage)
        {
        case lux::rdesc::EShaderType::VERTEX:   return "vertex";
        case lux::rdesc::EShaderType::FRAGMENT: return "fragment";
        case lux::rdesc::EShaderType::COMPUTE:  return "compute";
        default:                                return "undefined";
        }
    }

    int usage()
    {
        std::fprintf(stderr,
            "usage: lux_shader_emitter --in <file.lglsl> [--out <file>] [--print-meta] [--header]\n");
        return 2;
    }
} // namespace

int main(int argc, char** argv)
{
    std::string in_path;
    std::string out_path;
    bool print_meta = false;
    auto mode = lux::shadergen::lglsl::EEmitMode::Shader;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--in" && i + 1 < argc)        in_path = argv[++i];
        else if (arg == "--out" && i + 1 < argc)  out_path = argv[++i];
        else if (arg == "--print-meta")           print_meta = true;
        else if (arg == "--header")               mode = lux::shadergen::lglsl::EEmitMode::Header;
        else                                      return usage();
    }
    if (in_path.empty())
        return usage();

    std::ifstream in(in_path, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "lux_shader_emitter: cannot open '%s'\n", in_path.c_str());
        return 1;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string source = std::move(buf).str();

    const auto result = lux::shadergen::lglsl::emitCanonicalGlsl(source, mode);
    if (!result)
    {
        std::fprintf(stderr, "%s: %s\n", in_path.c_str(), result.error().c_str());
        return 1;
    }

    if (print_meta)
    {
        std::printf("stage=%s\n", toString(result->meta.stage));
        std::printf("entry=%s\n", result->meta.entry.c_str());
        for (const auto& v : result->meta.variants)
            std::printf("variant=%s\n", v.name.c_str());
    }

    if (!out_path.empty())
    {
        std::ofstream out(out_path, std::ios::binary);
        if (!out || !(out << result->glsl))
        {
            std::fprintf(stderr, "lux_shader_emitter: cannot write '%s'\n", out_path.c_str());
            return 1;
        }
    }
    return 0;
}
