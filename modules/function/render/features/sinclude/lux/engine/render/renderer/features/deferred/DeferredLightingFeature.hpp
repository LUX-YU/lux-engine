#pragma once
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/render/renderer/features/deferred/GBufferTypes.hpp>
#include <lux/engine/function/render/client/genops/DeferredLightingOperation.ops.hpp>
#include <lux/engine/function/render/client/resources/lighting/EShadowTechnique.hpp>
#include <lux/engine/render/gpu/lifecycle/FifOwned.hpp>
#include <cstdint>
#include <string>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

namespace lux::render
{

    /// 集群光照参数 UBO 的 C++ 侧镜像。
    ///
    /// **ABI 镜像**:GLSL 侧有 5 份手抄(cluster_build / cluster_count /
    /// cluster_scan / cluster_fill 四个 compute + deferred_lighting.frag),
    /// std140 布局必须逐字段对齐 —— mat4 view / mat4 proj / vec4 viewport /
    /// uvec4 grid / uvec4 limits。
    ///
    /// 此前它定义在 `init()` **函数体内部**,没有任何 sizeof 断言,而同模块另外
    /// 15 个 GPU 结构(ViewGpuData / InstanceCullMeta / GpuOctreeNode / …)全都有
    /// 精确断言 —— 它是唯一的裸奔者,而且因为是函数局部、任何头文件都引用不到,
    /// 是 6 份副本里最难被发现的一份。提到这里并补上断言。
    struct alignas(16) ClusterParamsGPU
    {
        float view[16];     ///< mat4
        float proj[16];     ///< mat4
        float viewport[4];  ///< vec4:width, height, near, far
        uint32_t grid[4];   ///< uvec4:x, y, z, enable_clustered
        uint32_t limits[4]; ///< uvec4:cluster_count, max_indices, point_count, spot_count
        std::int32_t camera_page[4];
        float camera_local_page_size[4];
    };
    static_assert(
        sizeof(ClusterParamsGPU) == 208,
        "ClusterParamsGPU 与 GLSL 侧 ClusterParamsUBO 的 std140 布局漂移"
        "(64+64+16+16+16+16+16=208);改这里必须同步 GLSL 字段表");
    static_assert(alignof(ClusterParamsGPU) == 16);

    /**
     * @brief Deferred lighting pass — reads GBuffer textures, applies PBR lighting,
     *        outputs an HDR color target (RGBA16F).
     *
     * Path A (SAMPLED):  Standalone pass sampling GBuffer as sampler2D textures.
     * Path B (INPUT_ATTACHMENT): Subpass input path (requires Phase A-2 infrastructure).
     *
     * Descriptor layout:
     *   Set 0: Scene (ViewGpuData[])
     *   Set 1: GBuffer (4× combined_image_sampler: albedo, normal, emissive, depth)
     *   Set 2: Light SSBOs + Shadow (same layout as forward Set 3)
     */
    class LightResources;
    class ViewCameraResource;

    class LUX_FUNCTION_PUBLIC DeferredLightingFeature : public RenderFeature
    {
    public:
        /// 真身在 comm 层 DeferredLightingOperation.hpp(客户端用枚举名配置,
        /// 不再抄裸数字);此处别名保持既有引用不变。
        using EReadMode = ELightingReadMode;

        struct Config
        {
            ShaderHandle vertex_shader{}; ///< fullscreen triangle
            /// Optional explicit override of the lighting fragment shader. If left
            /// invalid, init() picks the SPIR-V variant matching `technique`. To
            /// drive runtime technique switches, leave `fragment_shader` empty
            /// and update `technique` (then trigger a feature rebuild).
            ShaderHandle fragment_shader{};
            EShadowTechnique technique{EShadowTechnique::PCF};
            ShaderHandle cluster_build_shader{};
            ShaderHandle cluster_count_shader{};
            ShaderHandle cluster_scan_shader{};
            ShaderHandle cluster_fill_shader{};
            ShaderHandle cluster_clear_shader{};
            uint32_t enable_clustered{0};
            uint32_t cluster_x{16};
            uint32_t cluster_y{9};
            uint32_t cluster_z{24};
            uint32_t max_cluster_indices{1'048'576};
            EReadMode read_mode = EReadMode::SAMPLED;
            std::string color_output{"LitColor"}; ///< Name of the lit color target to create
            GBufferLayout gbuffer;
            std::string shadow_atlas{"ShadowAtlas"};
            std::string depth_target{"SceneDepth"};
        };

        explicit DeferredLightingFeature(Config cfg);
        ~DeferredLightingFeature() override;

        std::string_view name() const override
        {
            return "DeferredLighting";
        }

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void onDetachFromScene(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;
        void onFrameBegin(const FeatureFrameContext& ctx) override;

    private:
        /// 记忆化:本 feature 未声明 requires=lux.render.light.v1(无光场景装延迟
        /// 光照是合法的 —— 走 grid.w==0 回退),故判空保留、查找收敛为一次。
        LightResources* light_cache_{nullptr};
        /// 可选相机资源的记忆化缓存(见 resolveViewCameraOnce)。
        ViewCameraResource* cam_cache_{nullptr};

        [[nodiscard]] Expected<void> init();
        void destroy() noexcept;

        /// Whether clustered light culling should be active for the given live light
        /// count. Shared by addPasses (the compile-time decision) and onFrameBegin
        /// (the threshold-crossing watch) so the two never disagree. (perf)
        [[nodiscard]] bool clusteringWanted(uint32_t light_count) const noexcept
        {
            return (cfg_.enable_clustered != 0u) && light_count >= kClusteredLightThreshold &&
                   cluster_build_pipeline_.valid() && cluster_count_pipeline_.valid() &&
                   cluster_scan_pipeline_.valid() && cluster_fill_pipeline_.valid();
        }

        static constexpr uint32_t kClusteredLightThreshold = 16u;

        Config cfg_;
        GraphicsPipelineHandle lighting_pipeline_{kInvalidPipelineHandle};
        ComputePipelineHandle cluster_build_pipeline_{kInvalidComputePipelineHandle};
        ComputePipelineHandle cluster_count_pipeline_{kInvalidComputePipelineHandle};
        ComputePipelineHandle cluster_scan_pipeline_{kInvalidComputePipelineHandle};
        ComputePipelineHandle cluster_fill_pipeline_{kInvalidComputePipelineHandle};
        ComputePipelineHandle cluster_clear_pipeline_{kInvalidComputePipelineHandle};

        // GBuffer descriptor set layout (4× combined_image_sampler) — used by transient DS
        VkDescriptorSetLayout gbuffer_ds_layout_{VK_NULL_HANDLE};

        /// Resolved in init() from cfg_.read_mode × DeviceCaps:
        /// true = local-read merged-scope path (subpassLoad variant + inputRead
        /// declarations), false = classic SAMPLED path.
        bool effective_local_read_{false};
        VkDescriptorSetLayout cluster_ds_layout_{VK_NULL_HANDLE};
        VkDescriptorSetLayout cluster_clear_ds_layout_{VK_NULL_HANDLE};
        /// DescriptorService 采样器缓存的共享句柄 —— 服务持有生命周期。
        VkSampler gbuffer_sampler_{VK_NULL_HANDLE};

        // The clustering decision the render graph was last compiled with. addPasses
        // bakes the cluster passes (or not) from the live light count; onFrameBegin
        // re-checks the count each frame and requests a recompile when it crosses the
        // threshold, since light add/remove does not otherwise invalidate the graph. (perf)
        bool clustering_compiled_{false};
    };

} // namespace lux::render
