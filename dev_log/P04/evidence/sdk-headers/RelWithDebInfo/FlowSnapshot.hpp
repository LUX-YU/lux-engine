#pragma once
#include <lux/engine/editor/flowforge/FlowEdit.hpp>
#include <lux/engine/editor/sessions/SessionState.hpp>

namespace lux::editor::flowforge
{
    struct FlowSnapshotBudget final
    {
        std::size_t max_bytes{64 * 1024 * 1024};
    };
    class FlowSession;
    class FlowSnapshot final
    {
    public:
        [[nodiscard]] const lux::flowforge::FlowSource& source() const noexcept
        {
            return source_;
        }
        [[nodiscard]] sessions::ContentStamp content() const noexcept
        {
            return content_;
        }
        [[nodiscard]] sessions::ObservationVersion observed() const noexcept
        {
            return observed_;
        }

    private:
        friend class FlowReadView;
        // Every field in FlowSource is an owned scalar/string/vector. No metadata or vtable escapes.
        lux::flowforge::FlowSource source_;
        sessions::ContentStamp content_;
        sessions::ObservationVersion observed_;
    };
    class FlowReadView final
    {
    public:
        // A const FlowGraph is not deep-const (unique_ptr<Node> and RuntimeObject expose mutation).
        // Callbacks see a frozen source constructed and destroyed inside the same READING admission.
        template <class Fn>
        [[nodiscard]] auto withRead(Fn&& function, FlowSnapshotBudget budget = {}) const
            -> std::invoke_result_t<Fn, const lux::flowforge::FlowSource&>
        {
            using Result = std::invoke_result_t<Fn, const lux::flowforge::FlowSource&>;
            return gate_.withRead([&]() -> Result {
                auto frozen = captureAdmitted(budget);
                if (!frozen)
                    return lux::cxx::unexpected(frozen.error());
                return std::invoke(std::forward<Fn>(function), frozen->source());
            });
        }
        [[nodiscard]] FlowEditResult<FlowSnapshot> capture(FlowSnapshotBudget budget = {}) const;
        [[nodiscard]] FlowEditResult<std::string> encode() const;

    private:
        friend class FlowSession;
        FlowReadView(
            const FlowAuthoringSource& source,
            sessions::ContentStamp content,
            sessions::ObservationVersion observed,
            sessions::EditGate& gate
        ) noexcept
            : source_(source), content_(content), observed_(observed), gate_(gate)
        {}
        [[nodiscard]] FlowEditResult<FlowSnapshot> captureAdmitted(FlowSnapshotBudget) const;
        const FlowAuthoringSource& source_;
        sessions::ContentStamp content_;
        sessions::ObservationVersion observed_;
        sessions::EditGate& gate_;
    };
}
