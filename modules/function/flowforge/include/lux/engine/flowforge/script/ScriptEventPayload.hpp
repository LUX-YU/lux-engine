#pragma once

#include <lux/engine/flowforge/FlowNodeCatalog.hpp>
#include <lux/engine/function/script/ScriptEvent.hpp>

namespace lux::flowforge
{
    namespace detail
    {
        struct ScriptValueType;
    }

    // Clones retain the immutable type allocation used by captured pin values.
    class LUX_ENGINE_FLOWFORGE_PUBLIC ScriptEventPayload final
    {
    public:
        explicit ScriptEventPayload(const script::ScriptEventSourceDescription&) noexcept;
        ~ScriptEventPayload();
        ScriptEventPayload(const ScriptEventPayload&) noexcept = default;
        ScriptEventPayload& operator=(const ScriptEventPayload&) noexcept = default;
        ScriptEventPayload(ScriptEventPayload&&) noexcept;
        ScriptEventPayload& operator=(ScriptEventPayload&&) noexcept;

        [[nodiscard]] std::size_t descriptionBytes() const noexcept;
        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;

        [[nodiscard]] const script::ScriptEventSourceDescription& source() const noexcept
        {
            return source_;
        }

    private:
        script::ScriptEventSourceDescription source_;
        std::shared_ptr<const detail::ScriptValueType> type_;
    };

    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowNodeRegistration
    scriptEventRegistration(object::CodeLease code = object::CodeLease::builtin()) noexcept;
} // namespace lux::flowforge
