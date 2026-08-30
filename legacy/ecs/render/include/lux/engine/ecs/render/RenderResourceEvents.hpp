#pragma once
/**
 * @file RenderResourceEvents.hpp — 渲染资源驻留的共享词汇(驻留 T2)。
 *
 * 设计 SSOT:`.internal/render-resource-system-design.md`(J4/J10)。
 * 这里只有**纯数据**:域/阶段/失败类别枚举、失败结构、送达回调别名、
 * 与一个观察事件 struct。零行为、零 bus 依赖 —— RenderResourceFailed
 * 的发布在宿主(manager 经 setFailureSink 缝交出,宿主 publish,UI
 * 订阅;条例③:基础模块零事件概念)。
 *
 * deliver 契约(J2/J4,实现方与调用方共同遵守):
 *  - **恰好调用一次**:成功(句柄)或**终态**失败(ResourceFailure);
 *    瞬态失败在 manager 内部退避重试,不外泄;
 *  - 在泵相位(帧 OPEN)执行;闭包只捕 id 不捕指针,送达时验活 +
 *    验组件仍引用该资产(死实体送达 = 自然空操作)。
 */

#include <lux/engine/resource/identity/AssetId.hpp>

#include <cstdint>
#include <string>

namespace lux::ecs
{
    /// 资源域(上传能力注册表的键;子系统按域向 manager 注册)。
    enum class EResourceDomain : std::uint8_t
    {
        MESH,
        TEXTURE,
        MATERIAL,
        MATERIAL_INSTANCE,
    };

    /// 失败发生的阶段(诊断定位用)。
    enum class EResourceStage : std::uint8_t
    {
        LOAD,           ///< CPU 数据加载/解码
        COMPILE,        ///< 着色器编译(材质域)
        RESOLVE_DEPS,   ///< 依赖解析(贴图槽/父级)
        UPLOAD,         ///< 上传 RPC 往返
    };

    /// 失败类别:瞬态(环境会变好,内部重试)/终态(内容坏,兜底+一次
    /// 诊断,revision 变更解封)。deliver 只见终态(J4)。
    enum class EFailureClass : std::uint8_t
    {
        TRANSIENT,
        TERMINAL,
    };

    struct ResourceFailure
    {
        EResourceStage stage{EResourceStage::LOAD};
        EFailureClass  cls{EFailureClass::TERMINAL};
        std::string    reason;
    };

    // (曾有 `Delivered<H>` 类型化送达别名 —— J6-六修随 hooks 一并退役:
    //  等待回调统一为服务层的类型擦除形,类型化解包在胶水侧做。)

    // ── 句柄位打包(跨层约定:index<<32 | gen)─────────────────────────
    //
    // 服务/编排/状态表只认 `std::uint64_t bits`(类型无关);打包发生在
    // 域子服务(上传回执处),解包发生在每世界胶水(写 cache 组件处)。
    // 模板不点名句柄类型 —— 本头因此不必 include 渲染句柄头;约定
    // bits==0 与默认构造句柄(is_null)同义,回执以 0 表失败不冲突。

    template <class H>
    [[nodiscard]] constexpr std::uint64_t packHandleBits(const H& h) noexcept
    {
        return (static_cast<std::uint64_t>(h.index) << 32)
             | static_cast<std::uint64_t>(h.gen);
    }

    template <class H>
    [[nodiscard]] constexpr H unpackHandleBits(std::uint64_t bits) noexcept
    {
        H h{};
        h.index = static_cast<decltype(h.index)>(bits >> 32);
        h.gen   = static_cast<decltype(h.gen)>(bits & 0xffff'ffffu);
        return h;
    }

    /// 终态失败的**观察事件**(J10:不驱动任何链——deliver 已单独兜底,
    /// 本事件只做可见性:编辑器 toast / 资产浏览器标红)。宿主从 manager
    /// 的 setFailureSink 缝收到后 publish;UI 在 ui 泵订阅。
    struct RenderResourceFailed
    {
        lux::asset::asset_id_t id;
        EResourceDomain        domain;
        ResourceFailure        failure;
    };

} // namespace lux::ecs
