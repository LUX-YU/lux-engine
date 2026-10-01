#pragma once
#include <lux/engine/editor/flowforge/FlowSession.hpp>
namespace lux::editor::flowforge
{
    // A complete, unpublished replacement. Preparation may call domain extensions under READING;
    // adoption rechecks the captured content under the original edit gate before the owner swap.
    class PreparedFlowReload final
    {
    public:
        PreparedFlowReload(const PreparedFlowReload&) = delete;
        PreparedFlowReload& operator=(const PreparedFlowReload&) = delete;
        PreparedFlowReload(PreparedFlowReload&&) noexcept = default;
        PreparedFlowReload& operator=(PreparedFlowReload&&) noexcept = default;
        [[nodiscard]] static FlowEditResult<PreparedFlowReload> prepare(
            FlowSession& session,
            FlowAuthoringSource source,
            lux::flowforge::FlowSourceEnvironment environment = {},
            std::optional<sessions::ContentStamp> expected = {},
            sessions::SourceBinding binding = {}
        );
        [[nodiscard]] FlowEditResult<void> adopt(FlowSession& session);

    private:
        PreparedFlowReload(sessions::ContentStamp expected, std::unique_ptr<FlowSession> candidate)
            : expected_(expected), candidate_(std::move(candidate))
        {}
        sessions::ContentStamp expected_;
        std::unique_ptr<FlowSession> candidate_;
    };
}
