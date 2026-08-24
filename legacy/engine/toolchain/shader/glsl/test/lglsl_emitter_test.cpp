// lglsl_emitter_test — unit test for the .lglsl -> canonical GLSL emitter
// (self-checking, exit 0 = pass).
// Covers: pragma parsing / canonical injection for opaque and multi-line
//         block declarations / lines that already have a layout pass through
//         unchanged / failure paths for unregistered names, unterminated
//         blocks, and missing pragmas / injection-table diagnostics.
#include <lux/engine/toolchain/shader/lglsl/LglslEmitter.hpp>

#include <cstdio>
#include <string_view>

namespace
{
    int g_failures = 0;

    void check(bool cond, const char* what)
    {
        if (!cond)
        {
            std::printf("FAIL: %s\n", what);
            ++g_failures;
        }
    }

    bool contains(std::string_view haystack, std::string_view needle)
    {
        return haystack.find(needle) != std::string_view::npos;
    }
} // namespace

int main()
{
    using lux::shadergen::lglsl::emitCanonicalGlsl;

    // ── 1. main path: pragma + opaque injection + multi-line block
    //      injection + plain lines pass through unchanged ────────────
    {
        constexpr std::string_view src =
            "//! lux-shader stage=fragment entry=main\n"
            "//! lux-variant USE_EVSM\n"
            "#version 450\n"
            "uniform sampler2D uTex[];\n"
            "readonly buffer VertexPool {\n"
            "    uint words[];\n"
            "} luxVertexPools[];\n"
            "layout(push_constant) uniform PC { uint scene; uint view; };\n"
            "void main() {}\n";

        auto r = emitCanonicalGlsl(src);
        check(r.has_value(), "main path emits");
        if (r)
        {
            const auto& out = *r;
            check(out.meta.stage == lux::rdesc::EShaderType::FRAGMENT, "stage parsed");
            check(out.meta.entry == "main", "entry parsed");
            check(out.meta.variants.size() == 1 && out.meta.variants[0].name == "USE_EVSM",
                  "variant parsed");
            check(contains(out.glsl, "layout(set = 2, binding = 0) uniform sampler2D uTex[];"),
                  "opaque injection uses contract slot (uTex -> set2 b0)");
            check(contains(out.glsl, "layout(set = 7, binding = 0) readonly buffer VertexPool"),
                  "block injection uses contract slot (luxVertexPools -> set7 b0)");
            check(contains(out.glsl, "layout(push_constant) uniform PC { uint scene; uint view; };"),
                  "explicit layout line passes through untouched");
            check(contains(out.glsl, "#version 450"), "plain lines pass through");
            check(out.injected.size() == 2, "two injections recorded");
        }
    }

    // ── 2. failure path: an unregistered name (with a line number) ───
    {
        constexpr std::string_view src =
            "//! lux-shader stage=fragment\n"
            "uniform sampler2D uNotRegistered;\n";
        auto r = emitCanonicalGlsl(src);
        check(!r.has_value(), "unregistered resource fails");
        if (!r)
        {
            check(contains(r.error(), "line 2"), "error carries line number");
            check(contains(r.error(), "uNotRegistered"), "error names the resource");
        }
    }

    // ── 3. failure path: missing the lux-shader pragma ────────────────
    {
        auto r = emitCanonicalGlsl("#version 450\nvoid main() {}\n");
        check(!r.has_value(), "missing pragma fails");
    }

    // ── 4. failure path: an unterminated block / an unknown pragma
    //      directive ─────────────────────────────────────────────────
    {
        auto r = emitCanonicalGlsl(
            "//! lux-shader stage=compute\n"
            "buffer Foo {\n"
            "    uint x;\n");
        check(!r.has_value(), "unterminated block fails");
    }
    {
        auto r = emitCanonicalGlsl("//! lux-typo whatever\n");
        check(!r.has_value(), "unknown lux- pragma fails");
    }

    // ── 5. non-resource lines are unaffected: in/out and a "uniform"
    //      substring appearing inside a comment ────────────────────────
    {
        constexpr std::string_view src =
            "//! lux-shader stage=vertex\n"
            "layout(location = 0) in vec3 aPos;\n"
            "layout(location = 0) out vec2 vUV;\n"
            "// uniform sampler2D commentedOut;\n"
            "void main() {}\n";
        auto r = emitCanonicalGlsl(src);
        check(r.has_value(), "io/comment lines emit");
        if (r)
        {
            check(r->injected.empty(), "no injection for io/comment lines");
            check(contains(r->glsl, "// uniform sampler2D commentedOut;"),
                  "comment passes through verbatim");
        }
    }

    // ── 6. Header mode: no pragma is valid, declarations are still
    //      injected as usual, and a lux-shader pragma is an error ──────
    {
        using lux::shadergen::lglsl::EEmitMode;
        constexpr std::string_view hdr =
            "uniform sampler2D uTex[];\n"
            "vec3 sampleTex(uint i, vec2 uv) { return texture(uTex[i], uv).rgb; }\n";
        auto r = emitCanonicalGlsl(hdr, EEmitMode::Header);
        check(r.has_value(), "header mode emits without pragma");
        if (r)
        {
            check(contains(r->glsl, "layout(set = 2, binding = 0) uniform sampler2D uTex[];"),
                  "header mode injects canonical slot");
            check(r->meta.stage == lux::rdesc::EShaderType::UNDEFINED,
                  "header mode leaves stage undefined");
        }

        auto bad = emitCanonicalGlsl(
            "//! lux-shader stage=fragment\nuniform sampler2D uTex[];\n",
            EEmitMode::Header);
        check(!bad.has_value(), "lux-shader pragma inside a header fails");
    }

    // ── 6b. a trailing comment must not confuse classification (used to
    //       make the uBlur declaration wrongly enter block collection and
    //       swallow the following line) ─────────────────────────────────
    {
        using lux::shadergen::lglsl::EEmitMode;
        auto r = emitCanonicalGlsl(
            "uniform sampler2D uBlur;   // blurred mask (H+V)\n"
            "layout(location = 0) in vec2 vUV;\n",
            EEmitMode::Header);
        check(r.has_value(), "trailing comment on opaque decl emits");
        if (r)
        {
            check(contains(r->glsl,
                  "layout(set = 1, binding = 0) uniform sampler2D uBlur;   // blurred mask (H+V)"),
                  "comment preserved in output, slot injected");
            check(contains(r->glsl, "layout(location = 0) in vec2 vUV;"),
                  "following io line untouched");
            check(r->injected.size() == 1, "exactly one injection");
        }
    }

    // ── 7. a mergeable layout (memory qualifiers) merges into the slot's
    //      parentheses ─────────────────────────────────────────────────
    {
        using lux::shadergen::lglsl::EEmitMode;
        auto r = emitCanonicalGlsl(
            "layout(std430) readonly buffer VertexPool {\n"
            "    uint words[];\n"
            "} luxVertexPools[];\n",
            EEmitMode::Header);
        check(r.has_value(), "mergeable layout emits");
        if (r)
            check(contains(r->glsl,
                  "layout(set = 7, binding = 0, std430) readonly buffer VertexPool"),
                  "std430 merged after injected slot, original layout() replaced");

        // a layout with an explicit set passes through unchanged; push_constant is unaffected by the merge logic
        auto keep = emitCanonicalGlsl(
            "layout(set = 4, binding = 4, std430) readonly buffer M { uint x; } uM;\n"
            "layout(push_constant) uniform PC { uint a; };\n",
            EEmitMode::Header);
        check(keep.has_value(), "explicit layouts emit");
        if (keep)
        {
            check(contains(keep->glsl, "layout(set = 4, binding = 4, std430) readonly buffer M"),
                  "explicit set layout untouched");
            check(keep->injected.empty(), "explicit layouts are not counted as injections");
        }
    }

    if (g_failures == 0)
    {
        std::printf("lglsl_emitter_test: all checks passed\n");
        return 0;
    }
    std::printf("lglsl_emitter_test: %d check(s) FAILED\n", g_failures);
    return 1;
}
