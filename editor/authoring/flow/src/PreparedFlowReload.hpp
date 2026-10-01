#pragma once
#include "FlowSessionData.hpp"
namespace lux::editor::flowforge::detail
{
    class PreparedFlowReload final
    {
    public:
        [[nodiscard]] static FlowEditResult<PreparedFlowReload> prepare(
            FlowSession& session,
            FlowAuthoringSource source,
            lux::flowforge::FlowSourceEnvironment environment = {}
        )
        {
            struct Input final
            {
                lux::flowforge::FlowSourceEnvironment environment;
                FlowAuthoringSource source;
            } input{std::move(environment), std::move(source)};
            auto& owner = FlowSessionAccess::data(session);
            return owner.state.gate().withRead([&]() -> FlowEditResult<PreparedFlowReload> {
                auto admitted_input = std::move(input);
                if (!owner.state.binding())
                    return rejected(EFlowEditError::INVALID_SOURCE);
                auto candidate = FlowSession::create(
                    owner.state.id(),
                    owner.state.binding(),
                    std::move(admitted_input.source),
                    admitted_input.environment,
                    owner.limits
                );
                if (!candidate)
                    return lux::cxx::unexpected(candidate.error());
                return PreparedFlowReload{owner.content(), std::move(*candidate)};
            });
        }
        [[nodiscard]] FlowEditResult<void> adopt(FlowSession& session)
        {
            auto& owner = FlowSessionAccess::data(session);
            return owner.state.gate().withEdit([&](sessions::EditScope&) -> FlowEditResult<void> {
                if (owner.content() != expected_)
                    return rejected(EFlowEditError::STALE_CONTENT);
                auto& next = FlowSessionAccess::data(*candidate_);
                const auto state = next.content().state;
                using std::swap;
                swap(owner.environment, next.environment);
                swap(owner.code, next.code);
                swap(owner.source, next.source);
                swap(owner.history, next.history);
                if (!owner.state.loaded(state))
                    std::terminate();
                return {};
            });
        }

    private:
        PreparedFlowReload(sessions::ContentStamp expected, std::unique_ptr<FlowSession> candidate)
            : expected_(expected), candidate_(std::move(candidate))
        {}
        sessions::ContentStamp expected_;
        std::unique_ptr<FlowSession> candidate_;
    };
}
