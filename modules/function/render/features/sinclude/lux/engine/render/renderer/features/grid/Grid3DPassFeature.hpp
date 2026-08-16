#pragma once
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/function/render/client/features/grid/Grid3DPassTypes.hpp>
#include <cstdint>
#include <lux/engine/function/visibility.h>

#include <optional>
#include <string>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC Grid3DPassFeature : public RenderFeature
    {
    public:
        struct Config
        {
            ShaderHandle vertex_shader{};
            ShaderHandle fragment_shader{};
            std::string color_target{"SceneColor"};
            std::string depth_target{"SceneDepth"};
        };

        explicit Grid3DPassFeature(Config cfg);

        std::string_view name() const override { return "Grid3DPass"; }
        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;

        GraphicsPipelineHandle gridHandle() const noexcept { return grid_handle_; }

        void addPasses(RGBuilder& builder) override;

        // --- Per-frame feature phases ---------------------------------------
        void onFrameBegin(const FeatureFrameContext& ctx) override;

    public:
        /// 参数更新入口(comm handler 调)。写进 pending_，由 onFrameBegin 在
        /// 帧边界提交。
        ///
        /// (此前另有一个 onSyncSetGrid3DParams(SetGrid3DParamsCmd) 走同一个
        ///  pending_ —— 那是 sync-command 机制的残留,自始至终零调用,连同
        ///  Grid3DSyncCommands.hpp 一起退休。兄弟 Grid2D 根本没有这套东西。)
        void setGrid3DParams(const Grid3DParams& params) { pending_grid_params_ = params; }

    private:
        Config                      cfg_{};
        GraphicsPipelineHandle      grid_handle_{kInvalidPipelineHandle};
        Grid3DParams                  grid_params_{};
        std::optional<Grid3DParams>   pending_grid_params_;
    };

} // namespace lux::render
