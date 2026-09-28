#pragma once
#include "MaterialSessionData.hpp"

namespace lux::editor::material::detail
{
    class PreparedMaterialReload final
    {
    public:
        [[nodiscard]] static MaterialEditResult<PreparedMaterialReload> prepare(
            MaterialSession& session,
            lux::material::MaterialSource source,
            contracts::CodeLease code = contracts::CodeLease::builtin()
        )
        {
            auto& owner = MaterialSessionAccess::data(session);
            return owner.state.gate().withRead([&]() -> MaterialEditResult<PreparedMaterialReload> {
                if (!owner.state.binding())
                    return rejected(EMaterialEditError::INVALID_SOURCE);
                auto candidate = MaterialSession::create(
                    owner.state.id(),
                    owner.state.binding(),
                    std::move(source),
                    std::move(code),
                    owner.limits
                );
                if (!candidate)
                    return lux::cxx::unexpected(candidate.error());
                return PreparedMaterialReload{owner.content(), std::move(*candidate)};
            });
        }
        [[nodiscard]] MaterialEditResult<void> adopt(MaterialSession& session)
        {
            auto& owner = MaterialSessionAccess::data(session);
            return owner.state.gate().withEdit([&](sessions::EditScope&) -> MaterialEditResult<void> {
                if (owner.content() != expected_)
                    return rejected(EMaterialEditError::STALE_CONTENT);
                auto& next = MaterialSessionAccess::data(*candidate_);
                const auto state = next.content().state;
                using std::swap;
                swap(owner.code, next.code);
                swap(owner.source, next.source);
                swap(owner.history, next.history);
                if (!owner.state.loaded(state))
                    std::terminate();
                return {};
            });
        }

    private:
        PreparedMaterialReload(sessions::ContentStamp expected, std::unique_ptr<MaterialSession> candidate)
            : expected_(expected), candidate_(std::move(candidate))
        {}
        sessions::ContentStamp expected_;
        std::unique_ptr<MaterialSession> candidate_;
    };
}
