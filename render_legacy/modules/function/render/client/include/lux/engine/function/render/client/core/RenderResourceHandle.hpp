#pragma once
/// Cross-thread resource handle templates and common texture identity.

#include <lux/cxx/container/SlotMap.hpp>

#include <functional>
#include <type_traits>

namespace lux::render
{
    // =========================================================================
    //  Type tags — empty structs used solely to distinguish handle types.
    // =========================================================================
    struct TextureTag
    {};

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
    /// Only resources with identical remote/local slot identity may use this conversion.
    /// Textures have a separate typed identity table and must be resolved by TextureResources.
    template <typename To, typename From> constexpr To handle_cast(From h) noexcept
    {
        static_assert(!std::is_same_v<To, RTextureHandle> && !std::is_same_v<From, RTextureHandle>);
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
