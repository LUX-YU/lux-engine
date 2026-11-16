#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <tuple>

namespace lux::render
{
    namespace extension_detail
    {
        struct ResourceIdentity;
    }

    // Session-local value. Applications may compare/hash/store it, never interpret its encoding.
    // isNull() is a sentinel check, not proof of current existence; the actual storage checks generations.
    template <class Tag> class ResourceId final
    {
    public:
        ResourceId() noexcept = default;

        [[nodiscard]] bool isNull() const noexcept
        {
            return local_.isNull();
        }

        bool operator==(const ResourceId&) const noexcept = default;

        [[nodiscard]] bool operator<(const ResourceId& other) const noexcept
        {
            return std::tie(resource_.index, resource_.gen, local_.index, local_.gen) <
                   std::tie(other.resource_.index, other.resource_.gen, other.local_.index, other.local_.gen);
        }

        // Bucket selection only; equality always compares the complete identity.
        [[nodiscard]] std::uint64_t hash() const noexcept
        {
            return ((std::uint64_t(resource_.index) << 32) | local_.index) ^
                   (std::uint64_t(resource_.gen) * 0x9e3779b97f4a7c15ULL) ^ local_.gen;
        }

    private:
        friend struct extension_detail::ResourceIdentity;

        ResourceId(ResourceInstanceId resource, cxx::SlotKey<Tag> local) noexcept : resource_(resource), local_(local) {}

        ResourceInstanceId resource_;
        cxx::SlotKey<Tag> local_;
    };
} // namespace lux::render
