#pragma once

#include <lux/engine/description/Script.hpp>
#include <lux/engine/function/script/artifact/visibility.h>
#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/container/ScopeId.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::script
{
    inline constexpr std::string_view ScriptArtifactCanonicalName{"lux.script.artifact"};
    inline constexpr std::uint32_t ScriptArtifactPrimaryMagic{0x4153584CU};

    struct ScriptArtifactContentTag;
    using ScriptArtifactContentId = lux::cxx::ScopeId<ScriptArtifactContentTag>;

    enum class EScriptArtifactError : std::uint8_t
    {
        INVALID_DESCRIPTION,
        ALLOCATION_FAILURE,
    };

    class LUX_SCRIPT_ARTIFACT_PUBLIC ScriptArtifact final
    {
      public:
        ScriptArtifact(const ScriptArtifact&) = delete;
        ScriptArtifact& operator=(const ScriptArtifact&) = delete;
        ScriptArtifact(ScriptArtifact&& other) noexcept
            : description_(std::move(other.description_)), payload_(std::move(other.payload_)),
              export_index_(std::move(other.export_index_)), content_id_(std::exchange(other.content_id_, {}))
        {
        }
        ScriptArtifact& operator=(ScriptArtifact&& other) noexcept
        {
            if (this != &other)
            {
                description_ = std::move(other.description_);
                payload_ = std::move(other.payload_);
                export_index_ = std::move(other.export_index_);
                content_id_ = std::exchange(other.content_id_, {});
            }
            return *this;
        }

        // Identity of this immutable loaded content, not an AssetId, semantic hash or wire field.
        [[nodiscard]] ScriptArtifactContentId contentIdentity() const noexcept { return content_id_; }

        [[nodiscard]] static lux::cxx::expected<ScriptArtifact, EScriptArtifactError>
        create(lux::rdesc::Script description, std::vector<std::byte> payload) noexcept;

        [[nodiscard]] const lux::rdesc::Script& description() const noexcept
        {
            return description_;
        }

        [[nodiscard]] std::span<const std::byte> payload() const noexcept
        {
            return payload_;
        }

        [[nodiscard]] const lux::rdesc::ScriptFunction* findExport(ScriptSymbolId symbol) const noexcept;

      private:
        ScriptArtifact(lux::rdesc::Script description, std::vector<std::byte> payload) noexcept
            : description_(std::move(description)), payload_(std::move(payload))
        {
        }

        lux::rdesc::Script description_;
        std::vector<std::byte> payload_;
        std::unordered_map<ScriptSymbolId, std::size_t> export_index_;
        ScriptArtifactContentId content_id_;
    };

    class LUX_SCRIPT_ARTIFACT_PUBLIC ScriptArtifactAsset final
        : public lux::asset::TAsset<ScriptArtifact>
    {
    public:
        inline static constexpr std::string_view canonical_name = ScriptArtifactCanonicalName;
        inline static constexpr lux::asset::AssetTypeId asset_type =
            lux::asset::AssetTypeId::fromName(canonical_name);
        inline static constexpr std::uint32_t primary_magic = ScriptArtifactPrimaryMagic;
        inline static constexpr std::uint32_t legacy_type_tag = lux::asset::kNoLegacyAssetTypeTag;

        [[nodiscard]] static lux::cxx::expected<
            std::shared_ptr<const ScriptArtifactAsset>,
            lux::asset::AssetDecodeFailure
        > create(
            lux::asset::AssetInfo info,
            std::shared_ptr<const ScriptArtifact> data,
            std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary = {}
        ) noexcept;

    private:
        ScriptArtifactAsset(
            lux::asset::AssetInfo info,
            std::shared_ptr<const ScriptArtifact> data,
            std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary
        ) noexcept;
    };
} // namespace lux::script

namespace lux::asset
{
    template <>
    struct TAssetSerDeser<lux::script::ScriptArtifactAsset> final
    {
        [[nodiscard]] static LUX_SCRIPT_ARTIFACT_PUBLIC lux::cxx::expected<
            std::shared_ptr<const lux::script::ScriptArtifactAsset>,
            AssetDecodeFailure
        > decode(
            AssetId requested,
            lux::cxx::SharedBytes<> image,
            const AssetDecodeLimits& limits
        ) noexcept;

        [[nodiscard]] static LUX_SCRIPT_ARTIFACT_PUBLIC lux::cxx::expected<
            std::vector<std::byte>,
            AssetEncodeFailure
        > encode(
            const lux::script::ScriptArtifactAsset& asset,
            const AssetEncodeLimits& limits
        ) noexcept;
    };
} // namespace lux::asset
