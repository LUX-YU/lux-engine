#pragma once
/**
 * @file ShaderResources.hpp
 * @brief Manages compiled shader modules (VkShaderModule + reflection info).
 *
 * Registered in the ResourceRegistry so that RenderFeatures can
 * resolve ShaderHandle to ShaderObject during init().
 *
 * Usage — must<>, not find<>: RenderServer::init() emplaces this unconditionally
 * before any scene or feature exists, so absence is not a runtime possibility.
 *   auto& sr = registry.must<ShaderResources>();
 *   ShaderHandle h = sr.add(spirv, info);    // compiles SPIR-V → VkShaderModule
 *   const ShaderObject* obj = sr.get(h);     // CAN be null — a bad/unresolved handle
 *   sr.remove(h);
 */

#include <lux/engine/render/gpu/lifecycle/GPUResourceBase.hpp>
#include <lux/engine/render/gpu/ShaderObject.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/render/client/core/Errors.hpp> // Expected / renderFailure
#include <lux/engine/render/core/PreparedPipelineStages.hpp> // preparePipelineStages 的结果
#include <lux/engine/function/visibility.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace lux::rdesc
{
    class Shader;
}

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC ShaderResources final : public GPUResourceBase<ShaderResources, EGPUResourceType::Shader>
    {
    public:
        struct InitInfo
        {
            VkDevice device{VK_NULL_HANDLE};
            bool sparse_instance_pages{false};
        };

        ShaderResources() = default;
        ~ShaderResources();

        ShaderResources(const ShaderResources&) = delete;
        ShaderResources& operator=(const ShaderResources&) = delete;
        ShaderResources(ShaderResources&&) = default;
        ShaderResources& operator=(ShaderResources&&) = default;

        /// CANNOT FAIL — and that is why it returns void rather than bool.
        /// The body records the VkDevice and clears the bookkeeping containers;
        /// it makes no Vulkan call. Modules are created later, in add(), which
        /// does report failure. (Recorded explicitly so the "give every init a
        /// return value" sweep does not hand this one a constant-true bool.)
        void init(const InitInfo& info);
        void shutdown();

        [[nodiscard]] bool sparseInstancePages() const noexcept
        {
            return sparse_instance_pages_;
        }

        /// Compile SPIR-V into a VkShaderModule and register it.
        /// Returns a valid ShaderHandle on success, invalid on failure.
        [[nodiscard]] ShaderHandle add(const lux::rdesc::Shader& spirv, const lux::rdesc::ShaderInfo& info);

        /// Compile SPIR-V from a byte span (avoids heap allocation of Shader).
        [[nodiscard]] ShaderHandle add(std::span<const std::byte> spirv_bytes, const lux::rdesc::ShaderInfo& info);

        /// Register a pre-built ShaderObject. Returns a valid ShaderHandle.
        [[nodiscard]] ShaderHandle add(ShaderObject obj);

        /// Lookup a shader by handle. Returns nullptr for invalid/freed/stale handles.
        [[nodiscard]] const ShaderObject* get(ShaderHandle handle) const noexcept;

        /// This shader's SPIR-V bytes. Empty means the slot wasn't created via
        /// the deduplicating SPIR-V add() overload (the pre-built ShaderObject
        /// path has no source bytes).
        ///
        /// Domain merging needs to relocate a shader's descriptor positions from
        /// canonical to their merged position (SpirvPatcher). The bytes were
        /// already being kept around for dedup comparison — this just exposes
        /// them, without adding any new storage.
        [[nodiscard]] std::span<const std::byte> spirvBytes(ShaderHandle handle) const noexcept;

        /// 按引擎的搬运表把着色器的描述符位置搬到域合并后的位置,注册并返回补丁后的
        /// 着色器。
        ///
        /// 失败时返回具体的错误类型(源字节不可得 / SPIR-V 解析失败 / 三道门禁各自的
        /// 拒绝理由),实参里带着定位所需的数字。调用方**必须**处理:切换失败而继续按
        /// 未搬运的布局注册管线,会让同一个 pass 内的变体布局互不兼容,而 Vulkan 对此
        /// 不报错。
        ///
        /// @p info 必须是**原始**(未搬运)的反射:搬运表由它的原始位置加资源名算出。
        /// 元数据的搬运也在本函数内完成,因此它与 SPIR-V 补丁必然同步。
        ///
        /// 补丁结果走与普通 add() 相同的去重路径:多条管线把同一个着色器打成相同字节
        /// 时,只会有一个 VkShaderModule。
        [[nodiscard]] Expected<ShaderHandle>
        addMergedLayoutVariant(ShaderHandle source, const lux::rdesc::ShaderInfo& info);

        /// 把一条管线的**全部 stage** 一次切换到域合并布局,并取回它们的模块与反射。
        ///
        /// 这是 feature 侧应该用的入口。它一次解决四件事:
        ///   - 每个 stage 都过域合并切换,任一失败即整体报错(不存在部分切换的中间态);
        ///   - 全部 add() 在函数内部完成,返回的数据是拷贝,此后不受任何扩容影响;
        ///   - 「句柄先行」因此不再需要调用方记住 —— 写不出别的顺序;
        ///   - 「同一 pass 内的变体必须同批切换」这条约束随之自动成立:成功即全部一致。
        ///
        /// 空句柄的 stage 视为调用方的错误(缺 shader 应在解析阶段就报出来),返回
        /// shader.handle_stale。
        [[nodiscard]] Expected<PreparedPipelineStages> preparePipelineStages(std::span<const ShaderHandle> stages);

        // (原先这里还有 `mergedOrOriginal(ShaderHandle) -> ShaderHandle`:切换到域合并
        //  布局,失败就把源句柄还给你。它是全模块最后一处**主动丢弃错误**的地方 ——
        //  返回值分不清「本来就无需搬运」与「搬运失败」,调用方无从区分;而且它会让
        //  内部存储扩容,于是「句柄先行」这条不变量只能靠注释叮嘱。那条不变量踩过一次:
        //  registerFamilyPipelines 持有顶点着色器指针、逐个切换各家族的片元着色器,
        //  一次扩容重分配之后指针悬垂,拷出一份空反射,图材质变体拿到三套不同布局。
        //
        //  上面的 preparePipelineStages 一次解决这三件事:错误照实返回、返回的是拷贝、
        //  顺序由签名保证。四个调用点全部迁完之后它已删除。)

        /// Release one owner of the shader. The VkShaderModule is destroyed (and
        /// the slot freed + generation bumped) only when the LAST owner releases —
        /// identical SPIR-V is deduplicated to a single shared module (see `add`),
        /// so the module must outlive every material/instance that compiled it.
        void remove(ShaderHandle handle);

    private:
        /// One cached unique-SPIR-V → slot mapping. `bytes` is kept for an exact
        /// (collision-proof) compare on a hash hit; many materials that compile to
        /// byte-identical SPIR-V collapse onto one slot (→ one PSO downstream).
        struct SpirvCacheRec
        {
            std::vector<std::byte> bytes;
            uint32_t slot{0};
        };

        VkDevice device_{VK_NULL_HANDLE};
        bool sparse_instance_pages_{false};
        std::vector<ShaderObject> records_;
        std::vector<uint32_t> gens_;         ///< generation counter per slot
        std::vector<uint32_t> refcount_;     ///< owners per slot (parallel to records_); 0 = dead
        std::vector<std::size_t> slot_hash_; ///< SPIR-V hash per slot (0 = not from a deduped SPIR-V add)
        std::vector<uint32_t> free_;         ///< reusable slot indices
        std::unordered_map<std::size_t, std::vector<SpirvCacheRec>> spirv_cache_; ///< SPIR-V hash → slots
    };

} // namespace lux::render
