#pragma once
/// Cross-thread resource handle templates and common texture identity.

#include <lux/cxx/container/SlotMap.hpp>

#include <functional>

namespace lux::render
{
    // =========================================================================
    //  Type tags — empty structs used solely to distinguish handle types.
    // =========================================================================
    struct TextureTag
    {
    };

    // =========================================================================
    //  RenderResourceHandle<Tag> — alias to lux::cxx::SlotKey<Tag>
    // =========================================================================

    template <typename Tag> using RenderResourceHandle = lux::cxx::SlotKey<Tag>;

    // =========================================================================
    //  Concrete handle aliases
    // =========================================================================
    using RTextureHandle = RenderResourceHandle<TextureTag>;

    // =========================================================================
    //  跨线句柄 ↔ 内部句柄 的转换
    // =========================================================================
    /// R*Handle(跨线协议面)与 *Handle(引擎内部)是两套 tag 不同、布局相同的
    /// 句柄。它们**都是 L0 类型**,所以这个转换器也属于 L0。
    ///
    /// 它此前住在 L5 的**私有** RenderServerImpl.hpp 里 —— 于是每个只想转个
    /// 句柄的装配 TU 都得 include 整个服务端 Impl 头,把 Impl 的每个字段一并
    /// 拉进自己的可见范围。一个三行的类型工具不该有这种影响半径。
    template <typename To, typename From> constexpr To handle_cast(From h) noexcept
    {
        return To{h.index, h.gen};
    }

} // namespace lux::render

// =============================================================================
//  std::hash specialisation — delegates to SlotKey::Hash
// =============================================================================
namespace std
{
    template <typename Tag, typename I, typename G> struct hash<lux::cxx::SlotKey<Tag, I, G>>
    {
        size_t operator()(const lux::cxx::SlotKey<Tag, I, G>& h) const noexcept
        {
            return typename lux::cxx::SlotKey<Tag, I, G>::Hash{}(h);
        }
    };
} // namespace std
