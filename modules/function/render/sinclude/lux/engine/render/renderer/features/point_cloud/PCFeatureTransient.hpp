#pragma once
/**
 * @file PCFeatureTransient.hpp
 * @brief Point cloud render feature — Transient mode (current-frame-only).
 *
 * Unlike accumulative modes (Simple, GPUDriven, LOD, Splatting), this feature
 * renders only the point data received during the current frame.  If no data
 * arrives for a frame, nothing is drawn.
 *
 * Memory:      O(frame_data × framesInFlight) — only the latest frame, ring-buffered
 *              for frames-in-flight safety (the shared TransientVertexRing).
 * Performance: Single vkCmdDraw per frame, HOST_VISIBLE mapped buffer (zero-copy
 *              upload, no staging).
 * Rendering:   No slot management, no octree, no deferred destroy.
 *
 * @see IPointCloudFeature
 */

#include <lux/engine/render/renderer/features/point_cloud/IPointCloudFeature.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PointCloudGpuData.hpp>
#include <lux/engine/render/renderer/features/TransientVertexRing.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/function/visibility.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace lux::render
{
    // =========================================================================
    //  TransientPointCloudBuffer — scene-registry bridge between handler & feature
    // =========================================================================

    /**
     * @brief CPU-side staging area written by the upload handler and consumed
     *        by PCFeatureTransient::onFrameBegin().
     *
     * Both the handler and onFrameBegin execute on the server thread (sequential
     * within a single tick()), so no mutex is required.
     */
    struct TransientPointCloudBuffer
    {
        void replace(const GpuPointVertex* data, uint32_t count)
        {
            pending_.assign(data, data + count);
            dirty_ = true;
        }

        bool hasPending() const noexcept { return dirty_; }

        /// Move pending data out. Returns empty if nothing new arrived.
        std::vector<GpuPointVertex> take()
        {
            if (!dirty_) return {};
            dirty_ = false;
            return std::move(pending_);
        }

    private:
        std::vector<GpuPointVertex> pending_;
        bool dirty_{false};
    };

    // =========================================================================
    //  PCFeatureTransient
    // =========================================================================

    class LUX_FUNCTION_PUBLIC PCFeatureTransient final : public IPointCloudFeature
    {
    public:
        struct Config
        {
            ShaderHandle vertex_shader{};
            ShaderHandle fragment_shader{};
            float        point_size{3.0f};
            uint32_t     max_points{2'000'000};
            std::string  color_target{"SceneColor"};
            std::string  depth_target{"SceneDepth"};
        };

        explicit PCFeatureTransient(Config cfg);
        ~PCFeatureTransient() override;

        [[nodiscard]] EPointCloudMode mode() const noexcept override
        {
            return EPointCloudMode::TRANSIENT;
        }

        [[nodiscard]] std::string_view name() const override { return "PointCloudTransient"; }

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;
        void onFrameBegin(const FeatureFrameContext& ctx) override;
        void onDetachFromScene(RenderScene& scene) override;

        void setPointSize(float pixels) noexcept override
        {
            point_size_.store(pixels, std::memory_order_relaxed);
        }

    private:
        Config                 cfg_;
        GraphicsPipelineHandle pipeline_handle_{kInvalidPipelineHandle};
        std::atomic<float>     point_size_{3.0f};

        // Shared FIF vertex ring: sized to framesInFlight() and indexed by the REAL
        // frame_index (replaces the old fixed slots_[3] + private frame counter, which
        // could desync from the actual in-flight set).
        TransientVertexRing    ring_;
        uint32_t               active_slot_{0};
        uint32_t               draw_count_{0};

        TransientPointCloudBuffer* incoming_{nullptr};
    };

} // namespace lux::render
