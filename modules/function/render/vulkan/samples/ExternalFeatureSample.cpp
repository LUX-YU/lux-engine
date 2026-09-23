// A real plugin using only public SDK headers. The companion test checks GPU readback.
#include "ExternalFeature.hpp"
#include "ExternalShaders.hpp"
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/render/comm/server/FeatureRegistration.hpp>
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>
#include <lux/engine/function/render/client/RenderPluginExports.hpp>
#include <lux/engine/scene/RenderScenePluginExports.hpp>
#include <lux/engine/simulation/ecs/ComponentPluginExports.hpp>
#include <lux/engine/simulation/ecs/ComponentDecode.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <cstring>

#if defined(_WIN32)
#define SAMPLE_EXPORT __declspec(dllexport)
#else
#define SAMPLE_EXPORT __attribute__((visibility("default")))
#endif

namespace sample_ext
{
using namespace lux::render;
class Triangle final : public RenderFeature
{
  public:
    explicit Triangle(Tint color) : RenderFeature({"SampleExternal"}), color_(color) {}
    Expected<void> initAndAttachTo(RenderScene &) override
    {
        auto context = contextView();
        lux::rdesc::ShaderInfo vertex_info, fragment_info;
        vertex_info.entry_points.push_back({"main", lux::rdesc::EShaderType::VERTEX});
        fragment_info.entry_points.push_back({"main", lux::rdesc::EShaderType::FRAGMENT});
        fragment_info.push_constants.push_back({0, 24});
        const auto vertex = context.createShaderModule(std::as_bytes(std::span{kVertexShader}), vertex_info);
        const auto fragment = context.createShaderModule(std::as_bytes(std::span{kFragmentShader}), fragment_info);
        GraphicsPipelineTemplate pipeline;
        pipeline.vertex_shader = context.shaderModule(vertex);
        pipeline.fragment_shader = context.shaderModule(fragment);
        pipeline.cull_mode = VK_CULL_MODE_NONE;
        pipeline.depth_test_enable = false;
        pipeline.depth_write_enable = false;
        pipeline.debug_name = "ExternalTriangle";
        const std::array infos{context.shaderInfo(vertex), context.shaderInfo(fragment)};
        pipeline_ = context.registerGraphics(pipeline, infos);
        if (!pipeline_.valid()) return renderFailure<err::feature::ResourceInitFailed>();
        return {};
    }
    void addPasses(RGBuilder &builder) override
    {
        builder.addPass("ExternalTriangle", ERGPassType::GRAPHICS)
            .write(builder.referenceTexture("SceneColor"), ETextureRole::COLOR_ATTACHMENT)
            .setPipeline(pipeline_).stage(ERenderStage::Overlay)
            .setKernelFn([this](const PassRecordContext &context) {
                vkCmdPushConstants(context.cmd, context.pipeline_layout, context.pc_stage_flags,
                                   kViewPushPrefixSize, sizeof(color_), &color_);
                vkCmdDraw(context.cmd, 3, 1, 0, 0);
            });
    }
    EParamApply applyParams(const void *bytes, std::size_t size) override
    {
        if (size != sizeof(Tint)) return EParamApply::UNSUPPORTED;
        std::memcpy(&color_, bytes, sizeof(color_));
        return EParamApply::HOT;
    }
  private:
    Tint color_;
    GraphicsPipelineHandle pipeline_;
};

Expected<FeatureHandle> create(void *scene, const void *bytes, std::size_t size)
{
    auto color = decodeCommConfig<Tint>(bytes, size);
    if (!color) return lux::cxx::unexpected(color.error());
    return addFeature<Triangle>(scene, *color);
}
using Operations = FeatureOpRegistrar<ServerOp<ColorOp>>;

class TintStage final : public lux::scene::RenderSyncStage
{
  public:
    explicit TintStage(const lux::scene::RenderSyncStageCreateInfo &info)
        : registry_(info.registry), scene_(info.scene), feature_(info.feature_handle),
          operation_(info.catalog.paramSetOp("SampleExternal")),
          construct_(registry_.on_construct<Tint>().connect<&TintStage::changed>(*this)),
          update_(registry_.on_update<Tint>().connect<&TintStage::changed>(*this)),
          destroy_(registry_.on_destroy<Tint>().connect<&TintStage::changed>(*this)) {}
    bool hasPendingChanges() const noexcept override { return changed_; }
    void requestFullSync() noexcept override { changed_ = true; }
    lux::scene::ERenderSyncPrepareResult prepare(RenderProgramBuilder<> &builder) noexcept override
    {
        Tint color;
        for (const auto entity : registry_.view<Tint>()) { color = registry_.get<Tint>(entity); break; }
        const auto blob = builder.pushBlob(std::as_bytes(std::span{&color, 1}));
        builder.push(opcodes::CommandOp, operation_, SetFeatureParamsPayload{scene_, feature_, blob});
        return lux::scene::ERenderSyncPrepareResult::PREPARED_COMMANDS;
    }
    void commitPrepared() noexcept override { changed_ = false; }
    void discardPrepared() noexcept override {}
  private:
    void changed(lux::simulation::ecs::Registry &, lux::simulation::ecs::Entity) noexcept { changed_ = true; }
    lux::simulation::ecs::Registry &registry_;
    RenderSceneId scene_;
    FeatureHandle feature_;
    TypeId operation_;
    bool changed_{true}; // Includes components that existed before connection.
    entt::scoped_connection construct_, update_, destroy_;
};
}

extern "C" SAMPLE_EXPORT const lux::render::RenderPluginExports *lux_render_exports_v1() noexcept
{
    using namespace sample_ext;
    static const lux::render::RenderFeatureRegistration value{
        {&create, &Operations::registerAll, &Operations::unregisterAll, "SampleExternal", Operations::kParamSetOpIndex,
         kDescriptor, Operations::kOperationCount},
        lux::render::makeRenderFeatureConfigCodec<Tint>("sample.render.triangle.configuration", 1), true
    };
    static const lux::render::RenderPluginExports table{sizeof(table), 1, &value, 1};
    return &table;
}
extern "C" SAMPLE_EXPORT const lux::simulation::ecs::ComponentPluginExports *lux_component_exports_v1() noexcept
{
    using namespace lux::simulation::ecs;
    static const auto component = makeComponentSchema<sample_ext::Tint>(componentSchemaId("sample.Tint"), 1,
        EComponentSnapshotPolicy::COPY, {}, directComponentDecodeEmplace<sample_ext::Tint, 1>(),
        EComponentSemanticKind::IMPLEMENTATION_EXTENSION, directComponentCapture<sample_ext::Tint>(),
        directComponentDecodeValue<sample_ext::Tint, 1>(), directComponentValueCapture<sample_ext::Tint>(),
        directComponentReferences<sample_ext::Tint>());
    static const ComponentPluginExports table{sizeof(table), 1, &component, 1};
    return &table;
}
extern "C" SAMPLE_EXPORT const lux::scene::RenderScenePluginExports *lux_render_scene_exports_v1() noexcept
{
    using namespace lux::scene;
    static const ComponentObservationSpec observation{lux::cxx::typeToken<sample_ext::Tint>(), 7};
    static const RenderFeatureSceneBinding binding{
        lux::system::systemTypeId("lux.builtin.system.render"), sample_ext::kFeature, {&observation, 1},
        +[](const RenderSyncStageCreateInfo &info) noexcept
            -> lux::cxx::expected<std::unique_ptr<RenderSyncStage>, RenderSyncStageCreateFailure> {
            return std::make_unique<sample_ext::TintStage>(info);
        }
    };
    static const RenderScenePluginExports table{sizeof(table), 1, &binding, 1};
    return &table;
}
