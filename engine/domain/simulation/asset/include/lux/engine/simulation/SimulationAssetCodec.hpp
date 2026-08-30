#pragma once

#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/simulation/asset/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace lux::simulation
{
    inline constexpr std::string_view SimulationAssetCanonicalName{"lux.simulation.description"};
    inline constexpr std::uint32_t SimulationAssetPrimaryMagic{0x4453584CU};

    class LUX_ENGINE_SIMULATION_ASSET_PUBLIC SimulationAsset final
        : public lux::asset::TAsset<SimulationDescription>
    {
    public:
        inline static constexpr std::string_view canonical_name = SimulationAssetCanonicalName;
        inline static constexpr lux::asset::AssetTypeId asset_type =
            lux::asset::AssetTypeId::fromName(canonical_name);
        inline static constexpr std::uint32_t primary_magic = SimulationAssetPrimaryMagic;
        inline static constexpr std::uint32_t legacy_type_tag = lux::asset::kNoLegacyAssetTypeTag;

        [[nodiscard]] static lux::cxx::expected<
            std::shared_ptr<const SimulationAsset>,
            lux::asset::AssetDecodeFailure
        > create(
            lux::asset::AssetInfo info,
            std::shared_ptr<const SimulationDescription> data,
            std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary = {}
        ) noexcept;

    private:
        SimulationAsset(
            lux::asset::AssetInfo info,
            std::shared_ptr<const SimulationDescription> data,
            std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary
        ) noexcept;
    };
} // namespace lux::simulation

namespace lux::asset
{
    template <>
    struct TAssetSerDeser<lux::simulation::SimulationAsset> final
    {
        [[nodiscard]] static LUX_ENGINE_SIMULATION_ASSET_PUBLIC lux::cxx::expected<
            std::shared_ptr<const lux::simulation::SimulationAsset>,
            AssetDecodeFailure
        > decode(
            AssetId requested,
            lux::cxx::SharedBytes<> cooked_image,
            const AssetDecodeLimits& limits
        ) noexcept;

        [[nodiscard]] static LUX_ENGINE_SIMULATION_ASSET_PUBLIC lux::cxx::expected<
            std::vector<std::byte>,
            AssetEncodeFailure
        > encode(
            const lux::simulation::SimulationAsset& asset,
            const AssetEncodeLimits& limits
        ) noexcept;
    };
} // namespace lux::asset
