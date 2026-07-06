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
 * It owns its 2D sprite shaders, a color-only SceneColor pass (NO depth → pure painter
 * order, premultiplied-alpha blend), and a host-mapped vertex ring. Each frame the
 * submitted SpriteDraws are sorted, CPU-expanded into the ring slot for the current
 * frame-in-flight, and drawn.
 *
 * The ring has ONE slot per frame-in-flight and is indexed by the engine's real
 * `frame_index % framesInFlight()` — never a private frame counter, which would desync
 * from the actual in-flight set (feature disable/re-enable, skipped frames, multi-scene)
 * and let the CPU overwrite a slot the GPU is still reading.
 *
 * The ring is FEATURE-owned (feature VkBuffers, not a shared resource class): reusing
 * the shared TransientVertexSource would drag SkinningResources — a 3D resource — into a
 * 2D-only scene, breaking the verified 2D↔3D decoupling ("no 3D mesh arena").
 */

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>            // ShaderHandle
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp> // GraphicsPipelineHandle
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp> // SpriteDraw (drain buffer)
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <string>
#include <vector>

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
            std::string   color_target{"SceneColor"};   ///< the pass writes here (color-only)
            /// Per-frame sprite capacity (per ring slot). Sprites beyond it this frame are
            /// dropped (reported in FrameStats::dropped). A modest default keeps the ring
            /// small; a scene with more sprites raises it. (A future growable ring removes
            /// the fixed cap.)
            std::uint32_t max_sprites_per_frame{16'384};
            ShaderHandle  vertex_shader{};
            ShaderHandle  fragment_shader{};
        };

        /// Per-frame draw statistics: what the last onFrameBegin produced.
        struct FrameStats
        {
            std::uint32_t sprites{0};   ///< sprites drawn (after the max-per-frame clamp)
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
        static constexpr std::uint32_t kVertsPerSprite = 6;   ///< 2 triangles

        struct FrameSlot
        {
            VkBuffer      buffer{VK_NULL_HANDLE};
            VmaAllocation alloc{nullptr};
            void*         mapped{nullptr};
        };

        Config                 cfg_;
        GraphicsPipelineHandle pipeline_handle_{kInvalidPipelineHandle};

        VmaAllocator            allocator_{nullptr};
        std::vector<FrameSlot>  slots_;             ///< one host-mapped vertex buffer per frame-in-flight
        std::uint32_t           active_slot_{0};    ///< frame_index % slots_.size()
        std::uint32_t           draw_count_{0};     ///< vertices to draw this frame (6 * sprites)
        FrameStats              stats_{};

        Canvas2DResources*      ingest_{nullptr};   ///< scene-registry submission source
        std::vector<SpriteDraw> sprite_snapshot_;   ///< persistent drain buffer (capacity reused each frame)

        [[nodiscard]] lux::render::Expected<void> createSlotBuffers(std::uint32_t frames_in_flight);
        void destroySlotBuffers();
    };

} // namespace lux::render
