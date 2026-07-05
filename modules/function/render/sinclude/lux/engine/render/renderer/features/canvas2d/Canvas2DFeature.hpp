#pragma once
/**
 * @file Canvas2DFeature.hpp
 * @brief The SinglePerScene 2D draw-batch owner (design §3.1).
 *
 * Canvas2DFeature is the ONE render-side home of 2D drawing: the sprite / tilemap /
 * pixel-field bridges submit `LayerKey`-ordered draw batches, it (R2-03) stable-sorts
 * them and (R2-02) writes them in painter order straight to `SceneColor`. It is
 * SinglePerScene so there is no client-side name-routing ambiguity (design §0R V5).
 * The graph topology is STABLE (§3.2): `addPasses` declares ONE fixed pass; only the
 * per-frame batch CONTENT changes (uploaded in `onFrameBegin`), so sprite updates
 * never invalidate the render graph.
 *
 * R2-02 (this checkpoint) lands the pass + pipeline + upload + draw:
 *  - own 2D sprite shaders + a color-only SceneColor pass (NO depth → pure painter
 *    order), premultiplied-alpha blend;
 *  - per-frame the submitted SpriteDraws are CPU-expanded into a 3-slot host-mapped
 *    ring buffer and drawn.
 *
 * OWNERSHIP NOTE (deliberate divergence from design §3B's literal "reuse
 * TransientVertexSource"): recon found TransientVertexSource is (a) single-buffered
 * (a known FIF-safety bug, P1#9) and (b) OWNED BY SkinningResources — a 3D/skinning
 * resource. Reusing it would make a Canvas-only scene allocate SkinningResources,
 * breaking the 2D↔3D decoupling (R2-01's verified "no 3D mesh arena"). So Canvas2D
 * owns its OWN 3-slot ring buffer (mirroring TriOverlayTransientFeature) — NOT a new
 * heavy resource class, just feature-owned VkBuffers, and FIF-safe. See the 2D log.
 */

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>            // ShaderHandle
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp> // GraphicsPipelineHandle
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <string>

struct VmaAllocator_T;  using VmaAllocator  = VmaAllocator_T*;
struct VmaAllocation_T; using VmaAllocation = VmaAllocation_T*;

namespace lux::render
{
    struct Canvas2DResources;   // scene-registry ingest (Canvas2DResources.hpp)

    /// One expanded sprite-quad vertex (CPU-expanded from a SpriteDraw). 24 bytes,
    /// std-layout — matches the sprite.vert vertex input (loc0 vec2 pos, loc1 vec2 uv,
    /// loc2 uint premultiplied-RGBA8, loc3 uint bindless texture index).
    struct CanvasVertex
    {
        float         x, y;              ///< world XY (z = 0)
        float         u, v;              ///< atlas UV
        std::uint32_t rgba;              ///< premultiplied RGBA8
        std::uint32_t texture_bindless;  ///< bindless set-2 index, or kNoTexture (tint-only)
    };
    static_assert(sizeof(CanvasVertex) == 24);

    class LUX_FUNCTION_PUBLIC Canvas2DFeature final : public RenderFeature
    {
    public:
        struct Config
        {
            std::string   name{"Canvas2D"};
            std::string   color_target{"SceneColor"};    ///< the R2-02 pass writes here (color-only)
            std::uint32_t max_sprites_per_frame{100'000}; ///< ring-buffer capacity (design §3.3 sort budget)
            ShaderHandle  vertex_shader{};
            ShaderHandle  fragment_shader{};
        };

        /// Per-frame draw statistics (R2-03): what the last onFrameBegin produced.
        struct FrameStats
        {
            std::uint32_t sprites{0};   ///< sprites drawn (after the max-per-frame clamp)
            std::uint32_t batches{0};   ///< coalesced draw batches (sorted-adjacent, same texture)
            std::uint32_t dropped{0};   ///< sprites dropped because they exceeded max_sprites_per_frame
        };

        explicit Canvas2DFeature(Config cfg = {});
        ~Canvas2DFeature() override;

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void onFrameBegin(const FeatureFrameContext& ctx) override;
        void addPasses(RGBuilder& builder) override;
        void onDetachFromScene(RenderScene& scene) override;

        [[nodiscard]] const FrameStats& lastFrameStats() const noexcept { return stats_; }

    private:
        static constexpr std::uint32_t kBufferCount   = 3;             ///< FIF-safe ring
        static constexpr std::uint32_t kVertsPerSprite = 6;            ///< 2 triangles

        struct FrameSlot
        {
            VkBuffer      buffer{VK_NULL_HANDLE};
            VmaAllocation alloc{nullptr};
            void*         mapped{nullptr};
        };

        Config                 cfg_;
        GraphicsPipelineHandle pipeline_handle_{kInvalidPipelineHandle};

        VmaAllocator           allocator_{nullptr};
        FrameSlot              slots_[kBufferCount]{};
        std::uint32_t          active_slot_{0};
        std::uint32_t          frame_counter_{0};
        std::uint32_t          draw_count_{0};   ///< vertices to draw this frame (6 * sprites)
        FrameStats             stats_{};         ///< R2-03: last frame's sort/batch/drop counts

        Canvas2DResources*     ingest_{nullptr};  ///< scene-registry snapshot source

        void createSlotBuffers();
        void destroySlotBuffers();
    };

} // namespace lux::render
