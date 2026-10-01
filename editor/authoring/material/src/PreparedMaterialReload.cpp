#include <lux/engine/editor/material/PreparedMaterialReload.hpp>
#include "MaterialSessionData.hpp"
namespace lux::editor::material
{
    using namespace detail;
    MaterialEditResult<PreparedMaterialReload> PreparedMaterialReload::prepare(
        MaterialSession& session,
        lux::material::MaterialSource source,
        contracts::CodeLease code,
        std::optional<sessions::ContentStamp> expected,
        sessions::SourceBinding binding
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
            if (expected && (owner.content() != *expected || owner.state.binding() != binding))
                return rejected(EMaterialEditError::STALE_CONTENT);
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
    MaterialEditResult<void> PreparedMaterialReload::adopt(MaterialSession& session)
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
}
