#pragma once
// ============================================================================
//  FeatureFactory.hpp — 动态特性类型注册的函数指针表。
//
//  为什么在 L0 而不在 comm(L5):本结构只依赖 FeatureHandle / TypeId /
//  FeatureDescriptor,三者都是基础层类型;它描述的是"一个特性类型如何被创建
//  和注册"这条**契约**,不是线协议。它此前定义在 comm/RenderProtocol.hpp 里,
//  导致 47 个渲染层(L4)文件为了拿这一个结构就要包含整个协议头 —— 那个头带着
//  四十来个场景/视图/目标的线协议载荷,而渲染层一个都没用到(逐个核对为 0)。
//
//  comm/RenderProtocol.hpp 仍包含本头,对外接口不变。
// ============================================================================

#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>  // TypeId
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/FeatureDescriptor.hpp>
#include <lux/engine/function/render/client/core/Errors.hpp>                    // Expected<T>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lux::render
{
    /// 创建一个特性实例。返回 `Expected` 而不是裸句柄:装配失败的原因(载荷版本
    /// 不符、内置着色器缺失、依赖资源建不起来……)必须能到达发起 AddFeature 的
    /// 客户端。此前返回类型只有句柄,create_fn 内部拿到的每一个错误都无路可走,
    /// 只能就地打印然后交回一个无效句柄 —— 客户端只知道"装失败了",不知道为什么。
    using FeatureCreateFn        = Expected<FeatureHandle>(*)(void* scene, const void* param, size_t param_size);
    using FeatureRegisterOpsFn   = uint32_t(*)(void* dispatcher, TypeId* out_ops, uint32_t max_ops);
    using FeatureUnregisterOpsFn = void(*)(void* dispatcher, const TypeId* ops, uint32_t op_count);

    struct FeatureFactory
    {
        FeatureCreateFn        create_fn{};
        FeatureRegisterOpsFn   register_ops_fn{};
        FeatureUnregisterOpsFn unregister_ops_fn{};
        /// Stable feature name (== RenderFeature::name()). Lets a client key a
        /// name→{handle, ops} store (no hard-coded indices) and map the scene's
        /// queryFeatures() back to a feature's ops. A plugin supplies its own name
        /// here, so the editor discovers + edits it with zero compile-time coupling.
        const char*            name{""};
        /// Index into this factory's registered ops[] that is the GENERIC setParams
        /// op (a SetFeatureParamsPayload handler — FeatureParamsOperation.hpp), or -1
        /// when the feature exposes no editable params. Lets the settings panel push
        /// a reflected param blob to the right op BY NAME without knowing the
        /// feature's concrete type (so a plugin's params are editable too).
        int                    param_set_op_index{-1};
        /// Static type-level metadata: stable FeatureTypeId, declared dependencies /
        /// conflicts, capability flags. Default-empty (type == invalid) for
        /// factories that don't declare one — the SceneFeatureManager then treats the
        /// feature as today: caller-ordered, no dependency/conflict validation.
        FeatureDescriptor      descriptor{};
    };
    static_assert(std::is_trivially_copyable_v<FeatureFactory>);

    /// 把 create-fn 收到的原始附件解成具体的 CommConfig。
    ///
    /// 判据是**精确匹配**:载荷必须恰好是 `sizeof(Config)` 字节。长度对不上意味着
    /// 契约对不上 —— 要么发送侧送的根本不是这个类型,要么两侧的 CommConfig 定义已经
    /// 分叉。两种情况下再按字段读下去,读到的每一个值都是错位的,所以在拷贝之前就拦住,
    /// 并把「期望 / 实收」这一对数原样交回客户端:只说「长度不对」的话,对方还得自己
    /// 去猜是哪一边旧了。
    ///
    /// 此前这里是「长度不足就保留默认值」。那条宽松判据让「客户端的配置没送到」和
    /// 「客户端就是要默认值」在线上无法区分 —— config-less 的特性正是靠它送一个 1 字节
    /// 的空结构蒙混过关。现在那些调用点都送自己真正的 CommConfig 默认值(见
    /// ViewCameraCommTag / LightCommTag / SpatialCullCommConfig 等),判据才收得住。
    /// 版本演进靠 `comm_config_version` 字段,不靠长度余量。
    template <class Config>
    [[nodiscard]] Expected<Config> decodeCommConfig(const void* param, std::size_t param_size)
    {
        static_assert(std::is_trivially_copyable_v<Config>,
                      "CommConfig 必须可平凡拷贝 —— 它是直接 memcpy 过 comm 通道的。");

        if (param == nullptr || param_size != sizeof(Config))
            return renderFailure<err::comm::PayloadSizeMismatch>(
                static_cast<std::uint32_t>(sizeof(Config)),
                static_cast<std::uint32_t>(param_size));

        return *static_cast<const Config*>(param);
    }

    inline FeatureFactory makeSimpleFactory(FeatureCreateFn create_fn,
                                            const char* name = "",
                                            FeatureDescriptor descriptor = {}) noexcept
    {
        return FeatureFactory{
            create_fn,
            +[](void*, TypeId*, uint32_t) -> uint32_t { return 0u; },
            +[](void*, const TypeId*, uint32_t) {},
            name,
            -1,
            descriptor
        };
    }

} // namespace lux::render
