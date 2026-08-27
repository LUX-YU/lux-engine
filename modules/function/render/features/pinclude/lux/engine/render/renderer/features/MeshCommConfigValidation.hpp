#pragma once
#include <lux/engine/function/render/client/features/GpuDrivenMeshExtFlags.hpp>
// ============================================================================
//  MeshCommConfigValidation.hpp — shared create-fn preamble for the GPU-driven
//  mesh feature factories (DeferredGBuffer / ForwardMesh / MeshShadow).
//
//  Every mesh feature's create-fn validates its CommConfig payload identically:
//  size-check the blob, copy it, then reject an unsupported comm-config or
//  descriptor-layout version or any unknown extension flag. Only the Config type,
//  the version constants, and the known-flag mask differ — so the body lives here
//  once, parameterised by them. The op-registration half is already shared via
//  FeatureOpRegistrar; this is the create-fn half.
// ============================================================================

#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp> // resolveShaderStage / EBuiltinShader

#include <cstddef>
#include <cstdint>

namespace lux::render
{
    // ========================================================================
    //  内置着色器回填表 —— create-fn 的另一半样板
    //
    //  三个 mesh 族 handler(ForwardMesh / DeferredGBuffer / MeshShadow)在
    //  validate 之后各写一段完全同构的"逐字段回填内置着色器 + 逐字段校验":
    //  每字段一行回填、一个 has_shader lambda、一串 || 校验。
    //  普查(2026-07-28)确认恰有三种条目形状,表结构逐一覆盖:
    //
    //    固定    多数字段:空则回填指定内置,回填后必须可解析
    //    条件    cull shader:HZB 旗标命中选 _HZB 变体(着色器与管线布局在
    //            GpuDrivenMeshFeatureBase::initCommon 按同一旗标分支,必须成对)
    //    可选    Graph 族 frag(无内置,空=跳过该族)与已退役的 finalize
    //            (wire 兼容:空合法;非空必须可解析)
    //
    //  这张表同时就是生成器该产出的东西 —— 先手写收敛钉形状,后由
    //  meta-gen 从 CommConfig 的注解誊写(纪律:先样品,后模具)。
    // ========================================================================

    /// 一个 CommConfig 着色器字段的回填/校验描述。
    template <class Config> struct BuiltinShaderFill
    {
        ShaderHandle Config::* field; ///< CommConfig 里的句柄成员
        bool fill{true};              ///< false = 不回填(纯校验,如 Graph 族)
        EBuiltinShader builtin{};     ///< 回填用的默认内置
        /// Non-empty = conditional entry: pick builtin_alt when extension_flags
        /// contains these bits.
        GpuDrivenMeshExtFlags flag_mask{};
        EBuiltinShader builtin_alt{};
        bool optional{false}; ///< true = 空即合法;非空必须可解析
    };

    /// 按表回填 + 校验。任一必填字段解析不出模块(或可选字段非空却解析不出)即报错,
    /// 实参带上是哪个内置着色器 —— 这是三个 mesh 族 handler 共用的唯一回填入口。
    template <class Config, std::size_t N>
    [[nodiscard]] Expected<void>
    fillAndValidateBuiltinShaders(ShaderResources& shaders, Config& cc, const BuiltinShaderFill<Config> (&table)[N])
    {
        for (const BuiltinShaderFill<Config>& entry : table)
        {
            ShaderHandle& handle = cc.*(entry.field);

            // 条件条目:扩展旗标命中时换用备选内置(HZB 变体就是这么选的)。
            const EBuiltinShader builtin = (!entry.flag_mask.empty() && cc.extension_flags.containsAny(entry.flag_mask))
                                               ? entry.builtin_alt
                                               : entry.builtin;

            if (entry.fill)
            {
                auto resolved = resolveShaderStage(shaders, handle, builtin);
                if (!resolved)
                    return lux::cxx::unexpected(resolved.error());
                handle = *resolved;
            }

            // 可选字段留空是合法的;非空就必须解析得出模块。
            if (entry.optional && handle.isNull())
                continue;
            if (shaders.get(handle) == nullptr)
                return renderFailure<err::shader::BuiltinUnavailable>(static_cast<std::uint32_t>(builtin));
        }
        return {};
    }
    /// `validateMeshCommConfig` 的期望值。comm 版本与 layout 版本都是裸 uint32,散着
    /// 传相邻两个实参很容易传反而编译器帮不上忙;聚成一个具名结构后调用点用指派
    /// 初始化写出字段名,传反就看得见了。
    struct MeshCommConfigExpectation
    {
        std::uint32_t comm_version{};
        std::uint32_t descriptor_layout_version{};
        GpuDrivenMeshExtFlags known_ext_flags{};
    };

    /**
     * @brief Validate a mesh feature's CommConfig payload and copy it out.
     *
     *        Checks, in order: a non-null blob of exactly `sizeof(Config)` bytes,
     *        a matching comm_config_version, a matching descriptor_layout_version,
     *        and no unknown extension_flags. Each failure carries the expected and
     *        the actual value in the error's argument slots — the client compiled
     *        the other half of this contract, so it needs both numbers to know
     *        which side is stale.
     *
     *        @p Config must expose `comm_config_version`,
     *        `descriptor_layout_version`, and `extension_flags` — every mesh
     *        CommConfig does, and a future one that doesn't fails to compile here.
     */
    template <class Config>
    [[nodiscard]] Expected<void> validateMeshCommConfig(
        const void* param,
        std::size_t param_size,
        const MeshCommConfigExpectation& expected,
        Config& out_cc
    )
    {
        if (param == nullptr || param_size != sizeof(Config))
            return renderFailure<err::comm::PayloadSizeMismatch>(
                static_cast<std::uint32_t>(sizeof(Config)),
                static_cast<std::uint32_t>(param_size)
            );

        out_cc = *static_cast<const Config*>(param);

        if (out_cc.comm_config_version != expected.comm_version)
            return renderFailure<err::comm::ConfigVersionMismatch>(expected.comm_version, out_cc.comm_config_version);

        if (out_cc.descriptor_layout_version != expected.descriptor_layout_version)
            return renderFailure<err::comm::DescriptorLayoutVersionMismatch>(
                expected.descriptor_layout_version,
                out_cc.descriptor_layout_version
            );

        const GpuDrivenMeshExtFlags unknown = out_cc.extension_flags.withoutBitsIn(expected.known_ext_flags);
        if (!unknown.empty())
            return renderFailure<err::comm::UnknownExtensionFlags>(static_cast<std::uint32_t>(unknown.bits()));

        return {};
    }

    /// create-fn 前置的两步(校验载荷 + 回填内置着色器)合成一步,返回可用的 Config。
    /// 三个 mesh 族的 create-fn 只剩下一处失败出口,而不是各写两个 `if` —— 少一处
    /// 出口就少一个"忘了把错误传下去"的地方。
    template <class Config, std::size_t N>
    [[nodiscard]] Expected<Config> prepareMeshCommConfig(
        ShaderResources& shaders,
        const void* param,
        std::size_t param_size,
        const MeshCommConfigExpectation& expected,
        const BuiltinShaderFill<Config> (&shader_fills)[N]
    )
    {
        Config cc{};
        if (auto validated = validateMeshCommConfig(param, param_size, expected, cc); !validated)
            return lux::cxx::unexpected(validated.error());

        if (auto filled = fillAndValidateBuiltinShaders(shaders, cc, shader_fills); !filled)
            return lux::cxx::unexpected(filled.error());

        return cc;
    }

} // namespace lux::render
