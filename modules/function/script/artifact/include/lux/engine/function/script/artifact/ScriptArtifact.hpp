#pragma once

#include <lux/engine/description/Script.hpp>
#include <lux/engine/function/script/artifact/visibility.h>
#include <lux/engine/resource/asset/AssetCodecSet.hpp>

#include <lux/cxx/compile_time/expected.hpp>

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
        ScriptArtifact(ScriptArtifact&&) noexcept = default;
        ScriptArtifact& operator=(ScriptArtifact&&) noexcept = default;

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
    };

    [[nodiscard]] LUX_SCRIPT_ARTIFACT_PUBLIC lux::asset::AssetCodecDescriptor
    scriptArtifactCodecDescriptor(std::shared_ptr<const void> code_lifetime);
}
