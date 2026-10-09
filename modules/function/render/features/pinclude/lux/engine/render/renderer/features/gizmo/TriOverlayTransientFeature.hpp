#pragma once
/**
 * @file TriOverlayTransientFeature.hpp
 * @brief Triangle-overlay gizmo feature — transient mode (current-frame-only).
 *
 * Renders alpha-blended TRIANGLE_LIST primitives uploaded each frame.
 * If no data arrives for a frame, nothing is drawn.
 */

#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/render/features/gizmo/GizmoVertex.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/render/renderer/features/TransientVertexRing.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lux::render
{
    // =========================================================================
    //  TransientTriOverlayBuffer — scene-registry bridge
    // =========================================================================

    struct TransientTriOverlayBuffer
    {
        void replace(const GizmoVertex* data, uint32_t count)
        {
            pending_.assign(data, data + count);
            dirty_ = true;
        }

        bool hasPending() const noexcept
        {
            return dirty_;
        }

        std::vector<GizmoVertex> take()
        {
            if (!dirty_)
            {
                return {};
            }
            dirty_ = false;
            return std::move(pending_);
        }

    private:
        std::vector<GizmoVertex> pending_;
        bool dirty_{false};
    };

    // =========================================================================
    //  TriOverlayTransientFeature
    // =========================================================================

    class LUX_FUNCTION_PUBLIC TriOverlayTransientFeature final : public RenderFeature
    {
    public:
        struct Config
        {
            ShaderHandle vertex_shader{};
            ShaderHandle fragment_shader{};
            uint32_t max_vertices{200'000};
            std::string color_target{"SceneColor"};
            std::string depth_target{"SceneDepth"};
        };

        explicit TriOverlayTransientFeature(Config cfg);
        ~TriOverlayTransientFeature() override;

        [[nodiscard]] std::string_view name() const override
        {
            return "TriOverlayTransient";
        }

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;
        void onFrameBegin(const FeatureFrameContext& ctx) override;
        void onDetachFromScene(RenderScene& scene) override;

    private:
        Config cfg_;
        GraphicsPipelineHandle pipeline_handle_{kInvalidPipelineHandle};

        std::optional<TransientVertexRing> ring_;
        uint32_t active_slot_{0};
        uint32_t draw_count_{0};

        TransientTriOverlayBuffer* incoming_{nullptr};
    };

} // namespace lux::render
