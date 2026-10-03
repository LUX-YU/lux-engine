#include <lux/engine/scene/scripting/ScriptAssetAccess.hpp>
#include <AssetAbility.ability.generated.hpp>

namespace lux::scene::script
{
    namespace
    {
        using Traits = lux::script::TScriptAbilityTraits<AssetAbility>;
        using Completion = ScriptAssetScope::Completion;
        ScriptAssetScope& scope(void* context) noexcept { return *static_cast<ScriptAssetScope*>(context); }
        const Traits::Dispatch Dispatch{
            [](void*, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) noexcept {
                const std::array words{a, b, c, d};
                std::array<std::uint8_t, 16> bytes{};
                for (std::size_t i{}; i < bytes.size(); ++i)
                    bytes[i] = static_cast<std::uint8_t>(words[i / 4] >> (24 - (i % 4) * 8));
                return lux::asset::AssetId{bytes};
            },
            [](void*, lux::asset::AssetId id, std::uint32_t word) noexcept {
                if (word >= 4U) return std::uint32_t{};
                const auto bytes = id.bytes();
                std::uint32_t result{};
                for (std::size_t i{}; i < 4; ++i)
                    result = (result << 8U) | std::to_integer<std::uint8_t>(bytes[word * 4 + i]);
                return result;
            },
            [](void* context, lux::asset::AssetId id, Completion completion) noexcept -> lux::script::ScriptAbilityStartResult {
                auto result = scope(context).readAsset(id, std::move(completion));
                if (!result)
                    return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                        static_cast<std::int32_t>(result.error())
                    });
                return {};
            },
            [](void*, ScriptAssetReadOutcome value) noexcept { return value.succeeded(); },
            [](void*, ScriptAssetReadOutcome value) noexcept { return value.handle(); },
            [](void*, ScriptAssetReadOutcome value) noexcept { return static_cast<std::uint32_t>(value.errorDomain()); },
            [](void*, ScriptAssetReadOutcome value) noexcept { return value.errorCode(); },
            [](void* context, ScriptAssetHandle handle) noexcept {
                auto result = scope(context).describeAsset(handle);
                if (!result) return ScriptAssetInspection{{}, static_cast<std::uint32_t>(result.error())};
                return ScriptAssetInspection{*result, 0};
            },
            [](void*, ScriptAssetInspection value) noexcept { return value.error; },
            [](void*, ScriptAssetInspection value) noexcept { return value.value.id; },
            [](void*, ScriptAssetInspection value, std::uint32_t word) noexcept {
                if (word >= 2U) return std::uint32_t{};
                return static_cast<std::uint32_t>(value.value.image_bytes >> (word == 0U ? 32U : 0U));
            },
            [](void* context, ScriptAssetHandle handle, std::uint32_t high, std::uint32_t low, std::uint32_t count) noexcept {
                auto result = scope(context).copyAssetBytes(handle, (std::uint64_t{high} << 32U) | low, count);
                if (!result) return ScriptAssetBytes{{}, static_cast<std::uint32_t>(result.error())};
                return ScriptAssetBytes{*result, 0};
            },
            [](void*, ScriptAssetBytes value) noexcept { return value.error; },
            [](void*, ScriptAssetBytes value) noexcept { return value.value.size; },
            [](void*, ScriptAssetBytes value, std::uint32_t index) noexcept -> std::int32_t {
                if (index >= value.value.size) return -1;
                return std::to_integer<std::uint8_t>(value.value.bytes[index]);
            },
            [](void* context, ScriptAssetHandle handle) noexcept {
                // Script release is idempotent; it cannot release another instance's domain.
                // describeAsset reports precise validity; native clients retain the fallible release API.
                (void)scope(context).releaseAsset(handle);
            }
        };
    }
    lux::simulation::script::ScriptApiCapabilityPublication publishAssetAbility(ScriptAssetAccess& access) noexcept
    {
        return lux::simulation::script::publishScriptAbility(
            lux::script::ScriptAbilityBinding{&Traits::Description, &access, &Dispatch, Traits::ErasedMethods},
            &ScriptAssetAccess::prepareInstance
        );
    }
}
