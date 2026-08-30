#include <lux/engine/resource/asset/Asset.hpp>

#include <utility>

namespace lux::asset
{
    Asset::Asset(AssetInfo info, std::vector<AssetAuxiliaryPayload> auxiliary) noexcept
        : info_(std::move(info)), auxiliary_(std::move(auxiliary))
    {
    }

    Asset::~Asset() noexcept = default;

    AssetId Asset::id() const noexcept
    {
        return info_.id;
    }

    AssetTypeId Asset::type() const noexcept
    {
        return info_.type;
    }

    const AssetInfo& Asset::info() const noexcept
    {
        return info_;
    }

    std::span<const AssetAuxiliaryPayload> Asset::auxiliaryPayloads() const noexcept
    {
        return auxiliary_;
    }

    lux::cxx::SharedBytes<> Asset::auxiliaryPayload(AssetPayloadTag tag) const noexcept
    {
        for (const auto& payload : auxiliary_)
        {
            if (payload.tag == tag)
                return payload.bytes;
        }
        return {};
    }
} // namespace lux::asset
