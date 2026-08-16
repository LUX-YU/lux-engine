#pragma once
/**
 * @file LineListTransientFeature.hpp
 * @brief Line-list gizmo feature — transient mode (current-frame-only).
 *
 * Renders LINE_LIST primitives uploaded each frame. If no data arrives for
 * a frame, nothing is drawn. Follows the PCFeatureTransient pattern.
 */

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/render/client/features/gizmo/GizmoVertex.hpp>
#include <lux/engine/render/renderer/features/TransientVertexRing.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lux::render
{
    // =========================================================================
    //  TransientLineListBuffer — scene-registry bridge
    // =========================================================================

    struct TransientLineListBuffer
    {
        void replace(const GizmoVertex* data, uint32_t count)
        {
            pending_.assign(data, data + count);
            dirty_ = true;
        }

        bool hasPending() const noexcept { return dirty_; }

        std::vector<GizmoVertex> take()
        {
            if (!dirty_) return {};
            dirty_ = false;
            return std::move(pending_);
        }

    private:
        std::vector<GizmoVertex> pending_;
        bool dirty_{false};
    };

    // =========================================================================
    //  LineListTransientFeature
    // =========================================================================

    class LUX_FUNCTION_PUBLIC LineListTransientFeature final : public RenderFeature
    {
    public:
        struct Config
        {
            ShaderHandle vertex_shader{};
            ShaderHandle fragment_shader{};
            float        line_width{1.5f};
            uint32_t     max_vertices{200'000};
            std::string  color_target{"SceneColor"};
            std::string  depth_target{"SceneDepth"};
        };

        explicit LineListTransientFeature(Config cfg);
        ~LineListTransientFeature() override;

        [[nodiscard]] std::string_view name() const override { return "LineListTransient"; }

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;
        void onFrameBegin(const FeatureFrameContext& ctx) override;
        void onDetachFromScene(RenderScene& scene) override;

    private:
        Config                 cfg_;
        GraphicsPipelineHandle pipeline_handle_{kInvalidPipelineHandle};

        // Shared FIF vertex ring: sized to framesInFlight() and indexed by the REAL
        // frame_index (replaces the old fixed slots_[3] + private frame counter, which
        // could desync from the actual in-flight set).
        TransientVertexRing    ring_;
        uint32_t               active_slot_{0};
        uint32_t               draw_count_{0};

        TransientLineListBuffer* incoming_{nullptr};
    };

} // namespace lux::render
