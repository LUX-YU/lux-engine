#include <lux/engine/editor/flowforge/PreparedFlowReload.hpp>
#include "FlowSessionData.hpp"
namespace lux::editor::flowforge
{
    using namespace detail;
    FlowEditResult<PreparedFlowReload> PreparedFlowReload::prepare(
        FlowSession& session,
        FlowAuthoringSource source,
        lux::flowforge::FlowSourceEnvironment environment,
        std::optional<sessions::ContentStamp> expected,
        sessions::SourceBinding binding
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
            if (expected && (owner.content() != *expected || owner.state.binding() != binding))
                return rejected(EFlowEditError::STALE_CONTENT);
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
    FlowEditResult<void> PreparedFlowReload::adopt(FlowSession& session)
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
}
