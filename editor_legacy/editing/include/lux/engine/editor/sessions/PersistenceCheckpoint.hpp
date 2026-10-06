#pragma once

#include <lux/engine/editor/sessions/ContentStamp.hpp>
#include <lux/engine/editor/sessions/visibility.h>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <optional>
#include <string>

namespace lux::editor::sessions
{
    struct BoundSource final
    {
        asset::AssetId asset;
        std::string location; // Opaque source address; interpretation belongs to the persistence adapter.
        friend bool operator==(const BoundSource&, const BoundSource&) noexcept = default;
    };
    using SourceBinding = std::optional<BoundSource>;
    // Owning synchronous role result; no second mutable persistence state.
    struct SessionPersistenceView final
    {
        ContentStamp content;
        BindingRevision revision;
        SourceBinding source;
    };
    struct PersistedState final
    {
        editing::StateId state;
        BindingRevision binding;
        PublicationOrder publication;
        friend bool operator==(PersistedState, PersistedState) noexcept = default;
    };
    class LUX_EDIT_SESSIONS_PUBLIC PersistenceCheckpoint final
    {
    public:
        [[nodiscard]] const std::optional<PersistedState>& persisted() const noexcept
        {
            return persisted_;
        }
        [[nodiscard]] bool clean(editing::StateId current, BindingRevision binding) const noexcept;
        // Initial decoded content has an explicit baseline, before this binding's first publication.
        void loaded(editing::StateId state, BindingRevision binding) noexcept;
        void clear() noexcept
        {
            persisted_.reset();
        }
        [[nodiscard]] SessionResult<void> accept(
            editing::StateId current,
            BindingRevision binding,
            PersistedState published
        ) noexcept;

    private:
        std::optional<PersistedState> persisted_;
    };
}
