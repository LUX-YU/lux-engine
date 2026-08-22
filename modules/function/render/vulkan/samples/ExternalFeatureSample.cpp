/**
 * @file ExternalFeatureSample.cpp
 * @brief Reference "third-party" RenderFeature — the acceptance proof for the
 *        RenderFeature SDK (P3 facade + P4 creation tier).
 *
 * This file is NOT part of render.dll. It is compiled by
 *   modules/function/render/cmake/check_external_feature.ps1
 * with the EXACT flags an external consumer of the INSTALLED library sees: the
 * module's internal include dirs (sinclude / pinclude) are STRIPPED, leaving
 * only render/vulkan/include + the external SDKs. If it compiles, an outside author can
 * write a fully-functional feature using public headers alone.
 *
 * What it deliberately exercises, all from public headers:
 *   - subclass lux::render::RenderFeature and implement the pure-virtual addPasses
 *   - the narrow SDK facade: contextView() / sceneView()
 *       · createShaderModule / shaderModule / shaderInfo   (its OWN SPIR-V, not a builtin)
 *       · buildStandardGraphicsLayout / registerGraphics
 *       · device / vmaAllocator
 *       · retireBuffer / retireImage / retireImageView / retireSampler
 *       · sceneRegistry().ensure<T>()
 *   - the creation tier promoted to public in P4: RGBuilder, PassRecordContext,
 *     GraphicsPipelineTemplate
 *   - raw Vulkan + VMA, which a feature that authors its own pipelines/resources
 *     pulls IN ITSELF (by design — the base facade never forces vulkan.h on a
 *     consumer that does not author pipelines).
 *
 * It also shows the END-TO-END registration path (P4b): a FeatureFactory whose
 * create_fn installs the feature via the public addFeature(void* scene, ...)
 * — so a plugin never names the engine-internal RenderScene. A client registers
 * this factory through the public comm API (RenderFrameSession::registerFeatureType +
 * addFeature).
 */

#include <lux/engine/render/RenderFeature.hpp>                          // base + contextView()/sceneView()
#include <lux/engine/render/graph/RGBuilder.hpp>                        // RGBuilder / RGPassBuilder / ETextureRole
#include <lux/engine/function/render/graph/RGEnums.hpp>                          // ERGPassType / ERenderStage
#include <lux/engine/render/graph/PassRecordContext.hpp>                // PassRecordContext (kernel ctx)
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>      // GraphicsPipelineTemplate
#include <lux/engine/render/gpu/lifecycle/ResourceRegistry.hpp>   // sceneRegistry().ensure<T>()
#include <lux/engine/render/comm/server/FeatureRegistration.hpp>        // addFeature(void* scene, ...)
#include <lux/engine/render/graph/FrameExternalSync.hpp>                // addExternalGraphicsWait/Signal
#include <lux/engine/function/render/client/RenderProtocol.hpp>                    // FeatureFactory / makeSimpleFactory
#include <lux/engine/description/ShaderInfo.hpp>                        // lux::rdesc::ShaderInfo (createShaderModule)

#include <vulkan/vulkan.h>   // the author writes its own Vulkan pipeline state
#include <vk_mem_alloc.h>    // ...and manages its own GPU buffers/images via VMA

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sample_ext
{
    using namespace lux::render;

    /// A per-scene resource this feature owns (registry-first, get-or-create).
    struct SampleResource
    {
        uint32_t draws{0};
    };

    /// A self-contained overlay feature authored entirely against public headers.
    class SampleExternalFeature : public RenderFeature
    {
    public:
        struct Config
        {
            std::string name{"SampleExternal"};
            std::string color_target{"SceneColor"};
            std::string depth_target{"SceneDepth"};
            std::span<const std::byte> vertex_spirv{};    ///< author-compiled SPIR-V
            std::span<const std::byte> fragment_spirv{};
        };

        explicit SampleExternalFeature(Config cfg)
            : RenderFeature(RenderFeature::Config{cfg.name})
            , cfg_(std::move(cfg))
        {
        }

        // --- RenderFeature ---------------------------------------------------

        void initAndAttachTo(RenderScene & /*scene*/) override
        {
            // Everything below touches ONLY the narrow SDK facade — no
            // RenderContext / RenderScene / PipelineManager / ShaderResources.
            auto cv = contextView();

            // Load the feature's OWN shaders (its own SPIR-V — not an engine builtin).
            lux::rdesc::ShaderInfo vs_info{};
            lux::rdesc::ShaderInfo fs_info{};
            const ShaderHandle vsh = cv.createShaderModule(cfg_.vertex_spirv, vs_info);
            const ShaderHandle fsh = cv.createShaderModule(cfg_.fragment_spirv, fs_info);

            // Author its own pipeline template (this is where vulkan.h is needed —
            // and it is the FEATURE's include, not something the SDK forced on it).
            GraphicsPipelineTemplate tmpl{};
            tmpl.descriptor_set_count = 1;
            tmpl.topology             = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            tmpl.polygon_mode         = VK_POLYGON_MODE_FILL;
            tmpl.cull_mode            = VK_CULL_MODE_NONE;
            tmpl.front_face           = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            tmpl.use_dynamic_viewport = true;
            tmpl.use_dynamic_scissor  = true;
            tmpl.vertex_shader        = cv.shaderModule(vsh);
            tmpl.fragment_shader      = cv.shaderModule(fsh);

            std::vector<const lux::rdesc::ShaderInfo *> infos = {
                cv.shaderInfo(vsh),
                cv.shaderInfo(fsh)
            };
            tmpl.pipeline_layout = cv.buildStandardGraphicsLayout(
                tmpl.descriptor_set_count, infos, "SampleExternalLayout");
            pipeline_ = cv.registerGraphics(tmpl, infos);

            // A per-scene resource, owned via the public registry facade.
            sceneView().sceneRegistry().ensure<SampleResource>();

            // Author-owned GPU resources, created with the feature's own VMA usage.
            allocator_ = cv.vmaAllocator();
            device_    = cv.device();

            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size  = 256;
            bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            vmaCreateBuffer(allocator_, &bci, &aci, &buffer_, &buffer_alloc_, nullptr);

            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ici.imageType   = VK_IMAGE_TYPE_2D;
            ici.format      = VK_FORMAT_R8G8B8A8_UNORM;
            ici.extent      = {4, 4, 1};
            ici.mipLevels   = 1;
            ici.arrayLayers = 1;
            ici.samples     = VK_SAMPLE_COUNT_1_BIT;
            ici.usage       = VK_IMAGE_USAGE_SAMPLED_BIT;
            vmaCreateImage(allocator_, &ici, &aci, &image_, &image_alloc_, nullptr);

            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image            = image_;
            vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            vci.format           = VK_FORMAT_R8G8B8A8_UNORM;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(device_, &vci, nullptr, &view_);

            VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sci.magFilter = VK_FILTER_LINEAR;
            sci.minFilter = VK_FILTER_LINEAR;
            vkCreateSampler(device_, &sci, nullptr, &sampler_);
        }

        void addPasses(RGBuilder &builder) override
        {
            builder.addPass("SampleExternalPass", ERGPassType::GRAPHICS)
                .write(builder.referenceTexture(cfg_.color_target),
                       lux::render::ETextureRole::COLOR_ATTACHMENT)
                .write(builder.referenceTexture(cfg_.depth_target),
                       lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
                .setPipeline(pipeline_)
                .bindSceneDS(0)
                .setKernelFn(
                    [](const PassRecordContext &ctx)
                    {
                        if (ctx.view == nullptr)
                            return;
                        vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
                    })
                .setKernel("SampleExternalDraw")
                .stage(ERenderStage::Overlay);
        }

        void onDetachFromScene(RenderScene & /*scene*/) override
        {
            // Retire author-owned GPU resources through the facade — the feature
            // never names DeferredDestroyQueue, yet still gets FIF-safe deferred
            // destruction tied to the scene's frame serial.
            auto cv = contextView();
            if (buffer_)  cv.retireBuffer(buffer_, buffer_alloc_);
            if (image_)   cv.retireImage(image_, image_alloc_);
            if (view_)    cv.retireImageView(view_);
            if (sampler_) cv.retireSampler(sampler_);
            buffer_  = VK_NULL_HANDLE;
            image_   = VK_NULL_HANDLE;
            view_    = VK_NULL_HANDLE;
            sampler_ = VK_NULL_HANDLE;
        }

        void populateFrameContext(RGFrameContext &frame_ctx) override
        {
            // Zero-copy interop sync: the CUDA producer signaled ext_wait_sem_ to
            // frame_value_ after rendering into the imported image; make the graphics
            // submit WAIT for it before sampling, then SIGNAL ext_signal_sem_ so CUDA's
            // next frame can proceed (ping-pong). Handles are null in this compile-only
            // sample — a real feature imports them from CUDA via gapi exportHandle().
            if (ext_wait_sem_)
                lux::render::addExternalGraphicsWait(
                    frame_ctx, ext_wait_sem_, frame_value_, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
            if (ext_signal_sem_)
                lux::render::addExternalGraphicsSignal(
                    frame_ctx, ext_signal_sem_, frame_value_, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);
            ++frame_value_;
        }

    private:
        Config                 cfg_;
        GraphicsPipelineHandle pipeline_{};

        VmaAllocator allocator_{};
        VkDevice     device_{};
        VkBuffer      buffer_{};      VmaAllocation buffer_alloc_{};
        VkImage       image_{};       VmaAllocation image_alloc_{};
        VkImageView   view_{};
        VkSampler     sampler_{};

        // Imported-from-CUDA timeline semaphores (ping-pong) + the running value.
        VkSemaphore   ext_wait_sem_{};
        VkSemaphore   ext_signal_sem_{};
        uint64_t      frame_value_{0};
    };

    // ── Server-side factory glue (end-to-end registration, P4b) ──────────────
    //
    // The create_fn runs on the render thread when an addFeature request arrives.
    // It receives the scene type-erased as void* and installs the feature through
    // the public addFeature — never naming RenderScene.

    inline lux::render::Expected<lux::render::FeatureHandle>
    sampleCreateFn(void* scene, const void* param, std::size_t param_size)
    {
        SampleExternalFeature::Config cfg{};
        // A real plugin would decode its Config from the attachment with
        // lux::render::decodeCommConfig<MyCommConfig>(param, param_size) and return
        // that decode's error on failure; this sample keeps the defaults.
        (void)param;
        (void)param_size;
        return lux::render::addFeature<SampleExternalFeature>(scene, std::move(cfg));
    }

    /// The factory a client registers via RenderFrameSession::registerFeatureType().
    inline lux::render::FeatureFactory makeSampleFactory()
    {
        return lux::render::makeSimpleFactory(&sampleCreateFn, "SampleExternal");
    }

} // namespace sample_ext
