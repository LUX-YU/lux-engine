#pragma once
//
// AssetTypeRegistry — per-asset-type display metadata for the editor
// (tech-debt §2.12). Replaces the hand-written per-type `switch` in
// AssetBrowser so adding a new EAssetType is a single table row here instead
// of edits scattered across the panel(s). The metadata is editor/UI-layer
// (label, chip colour, a code-drawn glyph), kept out of the asset module.
//
// Icons are drawn procedurally with ImDrawList primitives (no image assets):
// a colour-filled rounded chip identifies the class at a glance, and a dark
// glyph on top distinguishes the shape (sphere = material, cube = mesh, …).
// A later pass can swap `draw_glyph` for a real texture-handle thumbnail
// without touching call sites.
//
#include <array>
#include <cstddef>

#include <imgui.h>

#include <lux/engine/asset/Asset.hpp>   // EAssetType

namespace lux::editor
{
    using IconGlyphFn = void (*)(ImDrawList* dl,
                                 const ImVec2& tl, const ImVec2& br, ImU32 col);

    struct AssetTypeDesc
    {
        const char* label;       ///< Terse tag, e.g. "TEX".
        const char* name;        ///< Human-readable, e.g. "Texture" (tooltips).
        ImU32       chip_color;  ///< Packed ARGB chip background.
        IconGlyphFn draw_glyph;  ///< Draws a dark glyph inside [tl,br].
    };

    namespace detail
    {
        // Inset [tl,br] by a fraction on every side.
        inline void inset(const ImVec2& tl, const ImVec2& br, float pad,
                          ImVec2& o_tl, ImVec2& o_br) noexcept
        {
            const float w = br.x - tl.x, h = br.y - tl.y;
            o_tl = { tl.x + w * pad, tl.y + h * pad };
            o_br = { br.x - w * pad, br.y - h * pad };
        }

        inline void glyphTexture(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, ImU32 c)
        {
            ImVec2 a, b; inset(tl, br, 0.16f, a, b);
            const float w = b.x - a.x, h = b.y - a.y;
            dl->AddRect(a, b, c, 1.5f, 0, 1.4f);                                   // photo frame
            dl->AddCircleFilled({ a.x + w * 0.30f, a.y + h * 0.30f }, w * 0.10f, c, 10); // sun
            dl->AddTriangleFilled({ a.x + w * 0.12f, b.y - 1.5f },                 // mountain
                                  { a.x + w * 0.50f, a.y + h * 0.55f },
                                  { a.x + w * 0.88f, b.y - 1.5f }, c);
        }

        inline void glyphMesh(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, ImU32 c)
        {
            ImVec2 a, b; inset(tl, br, 0.20f, a, b);
            const float w = b.x - a.x, o = w * 0.28f;
            const ImVec2 f0{ a.x, a.y + o }, f1{ b.x - o, a.y + o },
                         f2{ b.x - o, b.y }, f3{ a.x, b.y };            // front square
            const ImVec2 g0{ a.x + o, a.y }, g1{ b.x, a.y }, g2{ b.x, b.y - o }; // back
            dl->AddLine(f0, f1, c, 1.3f); dl->AddLine(f1, f2, c, 1.3f);
            dl->AddLine(f2, f3, c, 1.3f); dl->AddLine(f3, f0, c, 1.3f);
            dl->AddLine(f0, g0, c, 1.3f); dl->AddLine(f1, g1, c, 1.3f);
            dl->AddLine(f2, g2, c, 1.3f);
            dl->AddLine(g0, g1, c, 1.3f); dl->AddLine(g1, g2, c, 1.3f);
        }

        inline void glyphModel(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, ImU32 c)
        {
            // Filled isometric cube (3 visible faces, single colour + seams).
            ImVec2 a, b; inset(tl, br, 0.16f, a, b);
            const float cx = (a.x + b.x) * 0.5f;
            const float topY = a.y, midY = a.y + (b.y - a.y) * 0.32f, botY = b.y;
            const ImVec2 top{ cx, topY }, l{ a.x, midY }, r{ b.x, midY },
                         bl{ a.x, botY - (b.y - a.y) * 0.16f },
                         br2{ b.x, botY - (b.y - a.y) * 0.16f },
                         bot{ cx, botY };
            dl->AddQuadFilled(top, r, bot, l, c);                  // outer diamond body
            const ImU32 seam = IM_COL32(255, 255, 255, 40);
            dl->AddLine(top, bot, seam, 1.0f);                     // vertical seam
            dl->AddLine(l, r, seam, 1.0f);                         // horizontal seam
            (void)bl; (void)br2;
        }

        inline void glyphMaterial(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, ImU32 c)
        {
            ImVec2 a, b; inset(tl, br, 0.18f, a, b);
            const ImVec2 ctr{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
            const float r = (b.x - a.x) * 0.5f;
            dl->AddCircleFilled(ctr, r, c, 20);                                  // sphere
            dl->AddCircleFilled({ ctr.x - r * 0.32f, ctr.y - r * 0.34f },        // highlight
                                r * 0.22f, IM_COL32(255, 255, 255, 90), 12);
        }

        inline void glyphSkeleton(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, ImU32 c)
        {
            ImVec2 a, b; inset(tl, br, 0.20f, a, b);
            const ImVec2 j0{ a.x + (b.x - a.x) * 0.22f, a.y + (b.y - a.y) * 0.24f };
            const ImVec2 j1{ a.x + (b.x - a.x) * 0.78f, a.y + (b.y - a.y) * 0.76f };
            const float r = (b.x - a.x) * 0.16f;
            dl->AddLine(j0, j1, c, 2.0f);                  // bone shaft
            dl->AddCircleFilled(j0, r, c, 10);             // joints
            dl->AddCircleFilled(j1, r, c, 10);
        }

        inline void glyphAnimation(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, ImU32 c)
        {
            ImVec2 a, b; inset(tl, br, 0.24f, a, b);       // play triangle
            dl->AddTriangleFilled({ a.x, a.y }, { a.x, b.y },
                                  { b.x, (a.y + b.y) * 0.5f }, c);
        }

        inline void glyphShader(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, ImU32 c)
        {
            ImVec2 a, b; inset(tl, br, 0.20f, a, b);       // node + two links
            const ImVec2 ctr{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
            dl->AddCircle(ctr, (b.x - a.x) * 0.30f, c, 16, 1.4f);
            dl->AddLine({ a.x, ctr.y }, { ctr.x - (b.x - a.x) * 0.30f, ctr.y }, c, 1.3f);
            dl->AddLine({ ctr.x + (b.x - a.x) * 0.30f, ctr.y }, { b.x, ctr.y }, c, 1.3f);
        }

        inline void glyphGeneric(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, ImU32 c)
        {
            ImVec2 a, b; inset(tl, br, 0.24f, a, b);       // document with folded corner
            const float fold = (b.x - a.x) * 0.32f;
            dl->AddLine(a, { b.x - fold, a.y }, c, 1.3f);
            dl->AddLine({ b.x - fold, a.y }, { b.x, a.y + fold }, c, 1.3f);
            dl->AddLine({ b.x, a.y + fold }, b, c, 1.3f);
            dl->AddLine(b, { a.x, b.y }, c, 1.3f);
            dl->AddLine({ a.x, b.y }, a, c, 1.3f);
        }
    } // namespace detail

    /// Display metadata for @p type. Indexed by enum value; out-of-range maps
    /// to the UNKNOWN row.
    inline const AssetTypeDesc& assetTypeDesc(lux::asset::EAssetType type) noexcept
    {
        using lux::asset::EAssetType;
        // Order MUST match the EAssetType enum (TEXTURE..UNKNOWN). Indexed by enum
        // value — an out-of-order row mis-labels EVERY chip after it, so a new
        // EAssetType requires a row INSERTED at its ordinal + the size bump.
        // (W5c/W5d: the closure MATERIAL row was deleted and GRAPH_MATERIAL became
        // MATERIAL at ordinal 9 — this table mirrors the surviving 12-entry enum.)
        static const std::array<AssetTypeDesc, 14> table = { {
            { "TEX", "Texture",          IM_COL32( 64, 180, 220, 255), &detail::glyphTexture   },
            { "MDL", "Model",            IM_COL32(255, 215,  60, 255), &detail::glyphModel     },
            { "SHD", "Shader",           IM_COL32(160, 200, 240, 255), &detail::glyphShader    },
            { "MSH", "Mesh",             IM_COL32(120, 220,  90, 255), &detail::glyphMesh      },
            { "FNT", "Font",             IM_COL32(200, 200, 200, 255), &detail::glyphGeneric   },
            { "SND", "Sound",            IM_COL32(220, 220, 110, 255), &detail::glyphGeneric   },
            { "SCR", "Script",           IM_COL32(150, 200, 150, 255), &detail::glyphGeneric   },
            { "SKL", "Skeleton",         IM_COL32(180, 130, 240, 255), &detail::glyphSkeleton  },
            { "ANM", "Animation",        IM_COL32(220, 100, 200, 255), &detail::glyphAnimation },
            { "MAT", "Material",         IM_COL32( 90, 200, 180, 255), &detail::glyphMaterial  },
            { "MTI", "Material Instance",IM_COL32(120, 170, 220, 255), &detail::glyphMaterial  },
            { "SPA", "Sprite Atlas",     IM_COL32(240, 170,  90, 255), &detail::glyphTexture   },
            { "SPC", "Sprite Anim Clip", IM_COL32(240, 140, 160, 255), &detail::glyphAnimation },
            { "?",   "Unknown",          IM_COL32(120, 120, 120, 255), &detail::glyphGeneric   },
        } };
        const auto i = static_cast<std::size_t>(type);
        return (i < table.size()) ? table[i] : table.back();
    }

    /// Chip colour + glyph for a directory row (directories are not an
    /// EAssetType, so they live alongside the table).
    inline constexpr ImU32 kFolderChipColor = IM_COL32(220, 200, 120, 255);

    inline void drawFolderGlyph(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, ImU32 c)
    {
        ImVec2 a, b; detail::inset(tl, br, 0.18f, a, b);
        const float w = b.x - a.x, h = b.y - a.y;
        const float tabY = a.y + h * 0.22f;
        dl->AddLine({ a.x, tabY }, { a.x + w * 0.42f, tabY }, c, 1.3f);     // tab
        dl->AddLine({ a.x + w * 0.42f, tabY }, { a.x + w * 0.55f, a.y + h * 0.40f }, c, 1.3f);
        dl->AddRect({ a.x, a.y + h * 0.40f }, { b.x, b.y }, c, 1.5f, 0, 1.3f); // body
    }
}
