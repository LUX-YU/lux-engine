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
            struct ReloadInput final
            {
                contracts::CodeLease code; // Outlive every input node, including failed admission.
                lux::material::MaterialSource source;
            } input{std::move(code), std::move(source)};
            auto& owner = MaterialSessionAccess::data(session);
            return owner.state.gate().withRead([&]() -> MaterialEditResult<PreparedMaterialReload> {
                // Every admitted exit destroys the input before ReadScope restores admission.
                auto admitted_input = std::move(input);
                if (!owner.state.binding())
                    return rejected(EMaterialEditError::INVALID_SOURCE);
                auto candidate = MaterialSession::create(
                    owner.state.id(),
                    owner.state.binding(),
                    std::move(admitted_input.source),
                    admitted_input.code,
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
