#include <lux/engine/scene/ScenePackageCodec.hpp>

#include "LegacyEntitySceneAdapter.hpp"

namespace lux::scene
{
    ScenePackageCodecResult<std::vector<std::byte>> encodeScenePackage(
        const ScenePackage& package,
        const ScenePackageCodecLimits& limits) noexcept
    {
        const auto validated = validateScenePackage(package, limits);
        if (!validated)
            return lux::cxx::unexpected(validated.error());

        auto legacy = detail::toLegacyManifest(package, limits);
        if (!legacy)
            return lux::cxx::unexpected(legacy.error());
        auto encoded = lux::entity_scene::encodeEntitySceneManifest(
            *legacy,
            detail::legacyLimits(limits));
        if (!encoded)
            return lux::cxx::unexpected(
                detail::packageFailure(encoded.error()));
        return std::move(*encoded);
    }

    ScenePackageCodecResult<ScenePackage> decodeScenePackage(
        std::span<const std::byte> bytes,
        const ScenePackageCodecLimits& limits) noexcept
    {
        auto decoded = lux::entity_scene::decodeEntitySceneManifest(
            bytes,
            detail::legacyLimits(limits));
        if (!decoded)
            return lux::cxx::unexpected(
                detail::packageFailure(decoded.error()));

        auto package = detail::fromLegacyManifest(*decoded);
        const auto validated = validateScenePackage(package, limits);
        if (!validated)
            return lux::cxx::unexpected(validated.error());
        return package;
    }
} // namespace lux::scene
