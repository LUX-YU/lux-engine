#pragma once

#include <lux/engine/flowforge/graph/NodeBase.hpp>
#include <lux/engine/flowforge/visibility.h>
#include <lux/engine/function/script/ScriptEvent.hpp>

#include <memory>

namespace lux::flowforge
{
    class LUX_ENGINE_FLOWFORGE_PUBLIC ScriptEventAwaitNode final : public ExecIntermediateNode
    {
    public:
        ScriptEventAwaitNode(std::uint64_t id, const lux::script::ScriptEventSourceDescription& source);
        explicit ScriptEventAwaitNode(const lux::script::ScriptEventSourceDescription& source);
        ~ScriptEventAwaitNode() override;

        [[nodiscard]] const lux::script::ScriptEventSourceDescription& source() const noexcept
        {
            return source_;
        }

        [[nodiscard]] const DataOutPin& payloadPin() const noexcept
        {
            return *payload_pin_;
        }

    private:
        struct TypeStorage;

        lux::script::ScriptEventSourceDescription source_;
        std::unique_ptr<TypeStorage> type_;
        std::unique_ptr<DataOutPin> payload_pin_;
    };
}
