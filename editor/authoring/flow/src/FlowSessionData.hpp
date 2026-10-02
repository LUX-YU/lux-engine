#pragma once
#include <lux/engine/editor/editing/EditOperation.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/editor/flowforge/FlowSession.hpp>
#include "edits/FlowEditPreparation.hpp"

namespace lux::editor::flowforge
{
    struct FlowSession::Impl final
    {
        const std::thread::id owner{std::this_thread::get_id()};
        FlowSessionLimits limits;
        sessions::SessionState state;
        lux::flowforge::FlowSourceEnvironment environment;
        std::vector<contracts::CodeLease> code;
        FlowAuthoringSource source;
        std::unique_ptr<editing::EditHistory> history;
        Impl(sessions::SessionId id, sessions::SourceBinding binding, FlowSessionLimits policy)
            : limits(policy), state(id, std::move(binding))
        {}
        [[nodiscard]] sessions::ContentStamp content() const noexcept
        {
            const auto view = history->view();
            return {state.id(), view ? view->snapshot.current : editing::StateId{}};
        }
        [[nodiscard]] FlowEditResult<void> available() const noexcept
        {
            if (owner != std::this_thread::get_id())
                return lux::cxx::unexpected(sessions::ESessionError::WRONG_THREAD);
            if (state.admission() != sessions::EEditAdmission::AVAILABLE)
                return lux::cxx::unexpected(sessions::ESessionError::BUSY);
            return {};
        }
        [[nodiscard]] FlowEditResult<FlowEditReceipt> replay(bool forward);
    };
    namespace detail
    {
        struct FlowSessionAccess final
        {
            using Data = FlowSession::Impl;
            static Data& data(FlowSession& session) noexcept
            {
                return *session.impl_;
            }
        };
    }
}
