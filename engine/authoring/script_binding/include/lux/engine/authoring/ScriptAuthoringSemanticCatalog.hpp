#pragma once

#include <lux/engine/authoring/script_binding/visibility.h>
#include <lux/engine/function/script/ScriptSemantic.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::authoring
{
    enum class EScriptAuthoringSemanticCatalogError : std::uint8_t
    {
        INVALID_ENTRY,
        IDENTITY_CONFLICT,
        ALLOCATION_FAILURE,
    };

    struct ScriptAuthoringSemanticEntry final
    {
        std::string canonical_name;
        std::string type_id_hex;
        std::uint64_t type_id{};
        std::uint8_t abi_kind{LUX_SCRIPT_VK_VOID};
        std::uint32_t size{};
        std::uint32_t alignment{};
        std::vector<lux::script::EScriptPassMode> allowed_parameter_passes;
        lux::script::EScriptPassMode default_parameter_pass{
            lux::script::EScriptPassMode::VALUE};
        bool return_allowed{};

        friend bool operator==(
            const ScriptAuthoringSemanticEntry&,
            const ScriptAuthoringSemanticEntry&
        ) noexcept = default;
    };

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_BINDING_PUBLIC
    lux::cxx::expected<
        ScriptAuthoringSemanticEntry,
        EScriptAuthoringSemanticCatalogError>
    makeScriptAuthoringRecordEntry(
        std::string_view canonical_name,
        std::uint32_t size,
        std::uint32_t alignment
    ) noexcept;

    template <class Type>
    [[nodiscard]] lux::cxx::expected<
        ScriptAuthoringSemanticEntry,
        EScriptAuthoringSemanticCatalogError>
    makeScriptAuthoringRecordEntry() noexcept
    {
        return makeScriptAuthoringRecordEntry(
            lux::script::ScriptSemanticTypeTraits<Type>::CanonicalName,
            static_cast<std::uint32_t>(sizeof(Type)),
            static_cast<std::uint32_t>(alignof(Type))
        );
    }

    class LUX_ENGINE_AUTHORING_SCRIPT_BINDING_PUBLIC
        ScriptAuthoringSemanticCatalog final
    {
      public:
        [[nodiscard]] lux::cxx::expected<
            void,
            EScriptAuthoringSemanticCatalogError> add(
                ScriptAuthoringSemanticEntry entry
            ) noexcept;

        [[nodiscard]] std::span<const ScriptAuthoringSemanticEntry>
        entries() const noexcept;

        [[nodiscard]] const ScriptAuthoringSemanticEntry* find(
            std::string_view canonical_name
        ) const noexcept;

        [[nodiscard]] std::string canonicalJson() const;

      private:
        std::vector<ScriptAuthoringSemanticEntry> entries_;
    };

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_BINDING_PUBLIC
    lux::cxx::expected<
        ScriptAuthoringSemanticCatalog,
        EScriptAuthoringSemanticCatalogError>
    makeBaseScriptAuthoringSemanticCatalog() noexcept;
}
