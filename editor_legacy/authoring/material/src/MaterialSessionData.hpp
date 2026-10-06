#pragma once
#include <lux/engine/editor/editing/EditOperation.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/editor/material/MaterialSession.hpp>
#include "edits/MaterialEditPreparation.hpp"

namespace lux::editor::material
{
    struct MaterialSession::Impl final
    {
        const std::thread::id owner{std::this_thread::get_id()};
        MaterialSessionLimits limits;
        sessions::SessionState state;
        std::vector<lux::object::CodeLease> code;
        lux::material::MaterialSource source;
        std::unique_ptr<editing::EditHistory> history;
        Impl(sessions::SessionId id, sessions::SourceBinding binding, MaterialSessionLimits policy)
            : limits(policy), state(id, std::move(binding))
        {}
        [[nodiscard]] sessions::ContentStamp content() const noexcept
        {
            const auto view = history->view();
            return {state.id(), view ? view->snapshot.current : editing::StateId{}};
        }
        [[nodiscard]] MaterialEditResult<void> available() const noexcept
        {
            if (auto ready = state.gate().canEnter(); !ready)
                return lux::cxx::unexpected(ready.error());
            return {};
        }
        [[nodiscard]] MaterialEditResult<MaterialEditReceipt> replay(bool forward);
    };
    namespace detail
    {
        struct MaterialSessionAccess final
        {
            using Data = MaterialSession::Impl;
            static Data& data(MaterialSession& session) noexcept
            {
                return *session.impl_;
            }
        };
    }
}
