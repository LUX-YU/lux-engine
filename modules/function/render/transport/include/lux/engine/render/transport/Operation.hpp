#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <lux/engine/render/core/Descriptors.hpp>

namespace lux::render
{
    enum class ERenderLane : std::uint8_t
    {
        PROGRAM,
        CONTROL,
        UPLOAD
    };

    enum class ERenderOperationKind : std::uint8_t
    {
        POD,
        BULK,
        BLOB
    };

    struct BlobRef final
    {
        std::uint32_t offset{};
        std::uint32_t size{};
    };

    struct ExternalDataRef final
    {
        std::uint32_t attachment_index{};
        std::uint32_t offset{};
        std::uint32_t size{};
    };

    // Generated recursively from fields. Trivial copyability alone is not an
    // ownership proof: pointers, spans and string_views are deliberately rejected.
    template <typename T>
    struct TransportValue : std::bool_constant<std::is_arithmetic_v<T> || std::is_enum_v<T>> {};

    template <typename T, std::size_t Count>
    struct TransportValue<std::array<T, Count>> : TransportValue<T> {};

    template <> struct TransportValue<BlobRef> : std::true_type {};
    template <> struct TransportValue<ExternalDataRef> : std::true_type {};

    template <typename T>
    concept PacketValue = TransportValue<T>::value && std::is_trivially_copyable_v<T> &&
        std::is_standard_layout_v<T> && alignof(T) <= 64;

    template <typename T> struct RenderOpTraits;
    template <typename T> struct RenderReplyTraits;

    template <PacketValue T>
    inline constexpr auto renderReplyTypeId = renderDataTypeId(RenderReplyTraits<T>::name);

    struct RenderOperationDescriptor final
    {
        RenderDataDescriptor data;
        ERenderLane lane{};
        ERenderOperationKind kind{};
        std::optional<RenderDataDescriptor> reply;
    };

    template <PacketValue T>
    [[nodiscard]] constexpr RenderOperationDescriptor operationDescriptor() noexcept
    {
        using Traits = RenderOpTraits<T>;
        RenderOperationDescriptor result{
            {renderDataTypeId(Traits::name), Traits::name, Traits::wire_version,
                Traits::layout_version, sizeof(T), alignof(T)}, Traits::lane, Traits::kind, std::nullopt
        };
        if constexpr (!std::is_void_v<typename Traits::Reply>)
        {
            using Reply = typename Traits::Reply;
            static_assert(PacketValue<Reply>);
            using ReplyTraits = RenderReplyTraits<Reply>;
            result.reply = RenderDataDescriptor{
                renderDataTypeId(ReplyTraits::name), ReplyTraits::name, ReplyTraits::wire_version,
                ReplyTraits::layout_version, sizeof(Reply), alignof(Reply)
            };
        }
        return result;
    }
}
