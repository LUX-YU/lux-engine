#pragma once

#include <lux/engine/flowforge/FlowNodeCatalog.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityCatalog.hpp>

namespace lux::flowforge
{
    namespace detail
    {
        struct ScriptValueType;
    }

    // Immutable description/type storage is shared by payload clones. Restored RuntimeObjects
    // may still refer to that metadata; no clone invalidates it. Topology owns all actual pins.
    class LUX_ENGINE_FLOWFORGE_PUBLIC ScriptAbilityPayload final
    {
    public:
        explicit ScriptAbilityPayload(const ScriptAbilityNodeDescription&) noexcept;
        ~ScriptAbilityPayload();
        ScriptAbilityPayload(const ScriptAbilityPayload&) noexcept = default;
        ScriptAbilityPayload& operator=(const ScriptAbilityPayload&) noexcept = default;
        ScriptAbilityPayload(ScriptAbilityPayload&&) noexcept;
        ScriptAbilityPayload& operator=(ScriptAbilityPayload&&) noexcept;

        [[nodiscard]] const ScriptAbilityNodeDescription& description() const noexcept;
        [[nodiscard]] script::ScriptApiContractIdView contract() const noexcept;
        [[nodiscard]] script::ScriptApiMethodIdView method() const noexcept;
        [[nodiscard]] std::uint32_t expectedSchemaVersion() const noexcept;
        [[nodiscard]] std::uint64_t expectedSchemaHash() const noexcept;
        [[nodiscard]] std::size_t descriptionBytes() const noexcept;
        [[nodiscard]] script::EScriptApiMethodKind methodKind() const noexcept;
        [[nodiscard]] script::EScriptAbilityReceiverKind receiverKind() const noexcept;
        [[nodiscard]] std::span<const script::ScriptAbilityParameterDescription> parameters() const noexcept;
        [[nodiscard]] std::span<const script::ScriptAbilityValueDescription> results() const noexcept;
        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;

    private:
        std::shared_ptr<const detail::ScriptAbilityNodeStorage> description_;
        std::vector<std::shared_ptr<const detail::ScriptValueType>> types_;
    };

    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowNodeRegistration
    scriptAbilityRegistration(object::CodeLease code = object::CodeLease::builtin()) noexcept;
} // namespace lux::flowforge
