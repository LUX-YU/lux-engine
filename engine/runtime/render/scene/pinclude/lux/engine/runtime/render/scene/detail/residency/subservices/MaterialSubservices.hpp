#pragma once
/**
 * @file MaterialSubservices.hpp — 材质域 + 材质实例域子服务(驻留 T9/T10)。
 *
 * 四域里最复杂的两个,共享一份**域内记账**(MaterialArtifactStore):
 * 根材质编译出的 shader 句柄、上传副本(= 有效 params/贴图)、有效
 * render-state —— 子实例合并(父有效 ⊕ 本级 override)与销毁路径
 * (根材质要 destroyShader,实例绝不)都要读它。记账以最终
 * material handle bits 为键,不以 asset id 为键:同 id 换代时新旧 GPU
 * 副本可以短暂共存,迟到旧回执只会销毁自己那代。它不是驻留
 * 状态(生命周期在状态表),是域配方的工作数据。
 *
 * 与旧 GpuResourceCache 的本质区别(任务书 T9 原则):**贴图槽/父级
 * 依赖不在子服务里手写等待** —— 子服务经 `dependencies()` 申报,编排
 * 的依赖门(depsSender)把它们带到结算再 submit;submit 里只剩 stdexec
 * sender 配方(编译 gbuffer → 编译 forward(可选)→ 组装上传)。三段与
 * 外层驻留管道由 ResidencyAssembly 的同一 owner AsyncScope 结构化持有；
 * 不可取消的叶子 RPC 从出生起交给 reply reaper，stop 后的迟到 owner
 * 由 transaction RAII 补偿；
 * 实例域的单段 upload 也走同一种 reaper/强制收口，不留旁路。
 * 依赖句柄经装配注入的 HandleLookup 查表取位(READY 行→bits;失败/
 * 缺席→0 = 槽位留空,坏贴图不坏材质;父级 0 = 实例终败)。
 *
 * 疤痕保留:回执双条件验证(status+句柄);SPIR-V 反射**活取**(资产
 * 里烘的反射可能只覆盖子集,字节是唯一真相);forward 编译失败非致命
 * (回落家族 frag);实例链式派生用**根**的 shader 句柄(PSO 共享),
 * 环由编排祖先链检测。
 */

#include <lux/engine/runtime/render/scene/visibility.h>
#include <lux/engine/runtime/render/scene/detail/residency/RenderResourceService.hpp>

#include <lux/engine/function/render/client/core/ResourceHandle.hpp>                    // ShaderHandle
#include <lux/engine/function/render/client/resources/material/GraphMaterialData.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lux::asset  { class AssetManager; }
namespace lux::exec   { class AsyncScope; }
namespace lux::render
{
    class RenderControlSession;
    class RenderUploadClient;
    class FeatureCatalog;
    struct ShaderCompiledReply;
    struct MaterialUploadedReply;
}

namespace lux::runtime
{
    namespace detail
    {
        class MaterialRuntimeControl;
        template <class Reply> class OwnerReplyReaper;
    }

    /// 装配注入的「查已就绪句柄」回调(宿主接状态表:READY 行 → bits,
    /// 其余 → 0)。子服务据此把依赖解成 bindless/父级句柄 —— 依赖已被
    /// 编排带到结算,这里是纯查表。
    using HandleLookup =
        std::function<std::uint64_t(const lux::asset::asset_id_t&)>;

    /// 材质双域共享的域内记账(见文件头)。宿主构造一份,两个子服务
    /// 构造时同引一份;主线程独占访问。
    class MaterialArtifactStore
    {
    public:
        struct Artifacts
        {
            lux::render::ShaderHandle      gbuffer{};
            lux::render::ShaderHandle      forward{};
            lux::render::GraphMaterialData upload_copy{};
            std::uint32_t                  eff_alpha_mode{0};
            bool                           eff_double_sided{false};
            /// 根材质 = true(销毁时连 shader 一起);实例抄根句柄,恒 false。
            bool                           owns_shaders{false};
        };

        /// READY 句柄的精确域内记账。上传成功后先写入,
        /// 再把 bits 送给外层驻留表;若外层判定回执已过期,
        /// 临时 ResidentResourceLease 会以同一 bits 精确回收本条。
        std::unordered_map<std::uint64_t, Artifacts> by_handle_bits;
    };

    /// MATERIAL 域:烘焙图材质(gbuffer/forward SPIR-V + 参数默认 +
    /// 贴图槽)→ 编译 + 组装 + 上传。
    class LUX_RUNTIME_RENDER_SCENE_PUBLIC MaterialSubservice final
        : public IRenderResourceSubservice
    {
    public:
        MaterialSubservice(lux::render::RenderControlSession& control,
                           lux::render::RenderUploadClient upload,
                           lux::asset::AssetManager&          assets,
                           const lux::render::FeatureCatalog& catalog,
                           MaterialArtifactStore&             store,
                           HandleLookup                       lookup,
                           TryPostToMain                      post_main,
                           lux::exec::AsyncScope&             task_scope) noexcept;
        ~MaterialSubservice() override;

        MaterialSubservice(const MaterialSubservice&)            = delete;
        MaterialSubservice& operator=(const MaterialSubservice&) = delete;

        [[nodiscard]] lux::ecs::EResourceDomain domain() const override;
        [[nodiscard]] std::vector<ResourceDep>
        dependencies(const lux::asset::asset_id_t&) const override;

        void submit(const lux::asset::asset_id_t& id, SubmitDone done) override;
        void destroy(std::uint64_t handle_bits) noexcept override;

        /// 只读关停诊断:三段 sender 的叶子请求从出生起都由 reply reaper
        /// 持有,因此 scope stop 后仍可观察并补偿迟到 owner。
        [[nodiscard]] std::size_t pendingReplies() const noexcept override;
        [[nodiscard]] std::size_t pendingShaderReplies() const noexcept;
        [[nodiscard]] std::size_t pendingUploadReplies() const noexcept;
        [[nodiscard]] bool ownerControlsQuiescent() const noexcept override;
        void abandonPendingReplies() noexcept override;

    private:
        lux::asset::AssetManager*          assets_{nullptr};
        const lux::render::FeatureCatalog* catalog_{nullptr};
        HandleLookup                       lookup_{};
        lux::exec::AsyncScope*               task_scope_{nullptr};
        std::shared_ptr<detail::MaterialRuntimeControl> runtime_;
        std::shared_ptr<detail::OwnerReplyReaper<
            lux::render::ShaderCompiledReply>> shader_replies_;
        std::shared_ptr<detail::OwnerReplyReaper<
            lux::render::MaterialUploadedReply>> material_replies_;
    };

    /// MATERIAL_INSTANCE 域:链式派生(父可为材质或另一实例,UE MIC
    /// 同款)—— 父有效值 ⊕ 本级 override,用**根**shader 句柄上传保
    /// PSO 共享。父级门控由编排依赖门保证(submit 时父已结算)。
    class LUX_RUNTIME_RENDER_SCENE_PUBLIC MaterialInstanceSubservice final
        : public IRenderResourceSubservice
    {
    public:
        MaterialInstanceSubservice(
            lux::render::RenderControlSession& control,
            lux::render::RenderUploadClient upload,
            lux::asset::AssetManager&          assets,
            const lux::render::FeatureCatalog& catalog,
            MaterialArtifactStore&             store,
            HandleLookup                       lookup,
            TryPostToMain                      post_main) noexcept;
        ~MaterialInstanceSubservice() override;

        MaterialInstanceSubservice(const MaterialInstanceSubservice&) = delete;
        MaterialInstanceSubservice&
        operator=(const MaterialInstanceSubservice&)                  = delete;

        [[nodiscard]] lux::ecs::EResourceDomain domain() const override;
        [[nodiscard]] std::vector<ResourceDep>
        dependencies(const lux::asset::asset_id_t&) const override;

        void submit(const lux::asset::asset_id_t& id, SubmitDone done) override;
        void destroy(std::uint64_t handle_bits) noexcept override;

        [[nodiscard]] std::size_t pendingReplies() const noexcept override;
        [[nodiscard]] bool ownerControlsQuiescent() const noexcept override;
        void abandonPendingReplies() noexcept override;

    private:
        lux::asset::AssetManager*          assets_{nullptr};
        const lux::render::FeatureCatalog* catalog_{nullptr};
        HandleLookup                       lookup_{};
        std::shared_ptr<detail::MaterialRuntimeControl> runtime_;
        std::shared_ptr<detail::OwnerReplyReaper<
            lux::render::MaterialUploadedReply>> material_replies_;
    };

} // namespace lux::runtime
