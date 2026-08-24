#include <lux/engine/resource/asset/AssetCodecSet.hpp>

#include <algorithm>
#include <new>

namespace lux::asset
{
    struct AssetCodecSet::Impl final
    {
        std::vector<AssetCodecDescriptor> descriptors;
    };

    namespace
    {
        [[nodiscard]] bool sameCppType(
            lux::cxx::TypeToken left,
            lux::cxx::TypeToken right
        ) noexcept
        {
            return left.hash() == right.hash() && left.name() == right.name();
        }

        [[nodiscard]] bool magicConflicts(
            const AssetCodecDescriptor& left,
            const AssetCodecDescriptor& right
        ) noexcept
        {
            return left.primary_magic == right.primary_magic ||
                (left.legacy_magic != 0u &&
                    (left.legacy_magic == right.primary_magic ||
                     left.legacy_magic == right.legacy_magic)) ||
                (right.legacy_magic != 0u &&
                    right.legacy_magic == left.primary_magic);
        }
    } // namespace

    lux::cxx::expected<AssetCodecSet, EAssetCodecError>
    AssetCodecSet::build(
        std::vector<AssetCodecDescriptor> descriptors
    ) noexcept
    {
        try
        {
            for (std::size_t index{}; index < descriptors.size(); ++index)
            {
                const auto& descriptor = descriptors[index];
                if (descriptor.canonical_name.empty())
                    return lux::cxx::unexpected(EAssetCodecError::EMPTY_CANONICAL_NAME);
                if (!descriptor.type ||
                    descriptor.type != AssetTypeId::fromName(
                        descriptor.canonical_name
                    ) ||
                    descriptor.primary_magic == 0u ||
                    descriptor.cpp_payload_type.hash() == 0u ||
                    descriptor.cpp_payload_type.name().empty() ||
                    descriptor.decode == nullptr || descriptor.encode == nullptr)
                {
                    return lux::cxx::unexpected(EAssetCodecError::INVALID_DESCRIPTOR);
                }

                for (std::size_t other{}; other < index; ++other)
                {
                    const auto& existing = descriptors[other];
                    if (existing.type == descriptor.type)
                    {
                        return lux::cxx::unexpected(
                            existing.canonical_name == descriptor.canonical_name
                                ? EAssetCodecError::DUPLICATE_TYPE
                                : EAssetCodecError::TYPE_NAME_COLLISION
                        );
                    }
                    if (existing.canonical_name == descriptor.canonical_name)
                        return lux::cxx::unexpected(EAssetCodecError::TYPE_NAME_COLLISION);
                    if (magicConflicts(existing, descriptor))
                        return lux::cxx::unexpected(EAssetCodecError::DUPLICATE_MAGIC);
                    if (sameCppType(existing.cpp_payload_type, descriptor.cpp_payload_type))
                        return lux::cxx::unexpected(EAssetCodecError::DUPLICATE_CPP_TYPE);
                    if (existing.cpp_payload_type.hash() == descriptor.cpp_payload_type.hash())
                        return lux::cxx::unexpected(EAssetCodecError::CPP_TYPE_COLLISION);
                }
            }

            std::sort(
                descriptors.begin(),
                descriptors.end(),
                [](const auto& left, const auto& right)
                {
                    return left.type < right.type;
                }
            );
            auto impl = std::make_shared<Impl>();
            impl->descriptors = std::move(descriptors);
            return AssetCodecSet{std::move(impl)};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EAssetCodecError::OUT_OF_MEMORY);
        }
        catch (...)
        {
            return lux::cxx::unexpected(EAssetCodecError::INVALID_DESCRIPTOR);
        }
    }

    lux::cxx::expected<AssetCodecSet, EAssetCodecError>
    AssetCodecSet::extended(
        std::span<const AssetCodecDescriptor> descriptors
    ) const noexcept
    {
        try
        {
            std::vector<AssetCodecDescriptor> combined;
            if (impl_)
                combined = impl_->descriptors;
            combined.insert(combined.end(), descriptors.begin(), descriptors.end());
            return build(std::move(combined));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EAssetCodecError::OUT_OF_MEMORY);
        }
        catch (...)
        {
            return lux::cxx::unexpected(EAssetCodecError::INVALID_DESCRIPTOR);
        }
    }

    const AssetCodecDescriptor* AssetCodecSet::find(
        AssetTypeId type
    ) const noexcept
    {
        if (!impl_)
            return nullptr;
        const auto found = std::lower_bound(
            impl_->descriptors.begin(),
            impl_->descriptors.end(),
            type,
            [](const AssetCodecDescriptor& descriptor, AssetTypeId value)
            {
                return descriptor.type < value;
            }
        );
        return found != impl_->descriptors.end() && found->type == type
            ? std::addressof(*found)
            : nullptr;
    }

    const AssetCodecDescriptor* AssetCodecSet::findByMagic(
        std::uint32_t magic
    ) const noexcept
    {
        if (!impl_ || magic == 0u)
            return nullptr;
        const auto found = std::find_if(
            impl_->descriptors.begin(),
            impl_->descriptors.end(),
            [magic](const AssetCodecDescriptor& descriptor)
            {
                return descriptor.primary_magic == magic ||
                    descriptor.legacy_magic == magic;
            }
        );
        return found != impl_->descriptors.end() ? std::addressof(*found) : nullptr;
    }

    const AssetCodecDescriptor* AssetCodecSet::findByPayloadType(
        lux::cxx::TypeToken type
    ) const noexcept
    {
        if (!impl_)
            return nullptr;
        const auto found = std::find_if(
            impl_->descriptors.begin(),
            impl_->descriptors.end(),
            [type](const AssetCodecDescriptor& descriptor)
            {
                return sameCppType(descriptor.cpp_payload_type, type);
            }
        );
        return found != impl_->descriptors.end() ? std::addressof(*found) : nullptr;
    }

    std::span<const AssetCodecDescriptor>
    AssetCodecSet::descriptors() const noexcept
    {
        return impl_ ? std::span<const AssetCodecDescriptor>{impl_->descriptors}
                     : std::span<const AssetCodecDescriptor>{};
    }
} // namespace lux::asset
