#pragma once

#include <lux/engine/editor/sessions/PersistenceCheckpoint.hpp>
#include <lux/engine/editor/sessions/SessionPermits.hpp>
#include <functional>
#include <thread>
#include <utility>

namespace lux::editor::sessions
{
    enum class EEditAdmission : std::uint8_t
    {
        AVAILABLE,
        EDITING,
        CLOSING,
        REBINDING,
        READING
    };
    class EditGate;
    class LUX_EDIT_SESSIONS_PUBLIC EditScope final
    {
    public:
        ~EditScope() noexcept;
        EditScope(const EditScope&) = delete;
        EditScope& operator=(const EditScope&) = delete;
        EditScope(EditScope&&) = delete;
        EditScope& operator=(EditScope&&) = delete;

    private:
        friend class EditGate;
        explicit EditScope(EditGate& gate) noexcept : gate_(gate) {}
        EditGate& gate_;
    };
    class LUX_EDIT_SESSIONS_PUBLIC EditGate final
    {
    public:
        explicit EditGate(SessionId id) noexcept : id_(id) {}
        ~EditGate() noexcept;
        EditGate(const EditGate&) = delete;
        EditGate& operator=(const EditGate&) = delete;
        [[nodiscard]] EEditAdmission admission() const noexcept
        {
            return admission_;
        }
        template <class Fn> [[nodiscard]] auto withEdit(Fn&& function) -> std::invoke_result_t<Fn, EditScope&>
        {
            using Result = std::invoke_result_t<Fn, EditScope&>;
            if (auto entered = enter(EEditAdmission::EDITING); !entered)
                return Result{lux::cxx::unexpected(entered.error())};
            EditScope scope{*this};
            return std::invoke(std::forward<Fn>(function), scope);
        }

        // Protect the entire synchronous codec call, including callback-owned value destruction.
        // The scope cannot escape and does not confer editing or persistence authority.
        template <class Fn> [[nodiscard]] auto withRead(Fn&& function) -> std::invoke_result_t<Fn>
        {
            using Result = std::invoke_result_t<Fn>;
            if (auto entered = enter(EEditAdmission::READING); !entered)
                return Result{lux::cxx::unexpected(entered.error())};
            ReadScope scope{*this};
            return std::invoke(std::forward<Fn>(function));
        }

    private:
        class ReadScope final
        {
        public:
            explicit ReadScope(EditGate& gate) noexcept : gate_(gate) {}
            ~ReadScope() noexcept
            {
                gate_.leave(gate_.id_, EEditAdmission::READING);
            }
            ReadScope(const ReadScope&) = delete;
            ReadScope& operator=(const ReadScope&) = delete;

        private:
            EditGate& gate_;
        };
        friend class EditScope;
        friend class SessionState;
        friend class SessionStore;
        friend class ClosePermit;
        friend class BindingChangePermit;
        [[nodiscard]] SessionResult<void> enter(EEditAdmission admission) noexcept;
        void leave(SessionId id, EEditAdmission admission) noexcept;
        const std::thread::id owner_{std::this_thread::get_id()};
        SessionId id_;
        EEditAdmission admission_{EEditAdmission::AVAILABLE};
    };
    class LUX_EDIT_SESSIONS_PUBLIC SessionState final
    {
    public:
        explicit SessionState(SessionId id, SourceBinding binding = {}) noexcept
            : id_(id), binding_(std::move(binding)), gate_(id)
        {}
        [[nodiscard]] SessionId id() const noexcept
        {
            return id_;
        }
        [[nodiscard]] const SourceBinding& binding() const noexcept
        {
            return binding_;
        }
        [[nodiscard]] BindingRevision bindingRevision() const noexcept
        {
            return binding_revision_;
        }
        [[nodiscard]] ObservationVersion observed() const noexcept
        {
            return observed_;
        }
        [[nodiscard]] const PersistenceCheckpoint& checkpoint() const noexcept
        {
            return checkpoint_;
        }
        [[nodiscard]] EditGate& gate() noexcept
        {
            return gate_;
        }
        [[nodiscard]] EEditAdmission admission() const noexcept
        {
            return gate_.admission();
        }
        void contentChanged() noexcept;
        [[nodiscard]] SessionResult<void> loaded(editing::StateId state) noexcept;
        [[nodiscard]] SessionResult<void> accept(
            ContentStamp current,
            ContentStamp captured,
            BindingRevision binding,
            PublicationOrder order
        ) noexcept;
        [[nodiscard]] SessionResult<ClosePermit> prepareClose(ContentStamp current, ContentStamp expected) noexcept;
        [[nodiscard]] SessionResult<BindingChangePermit> prepareBindingChange(
            ContentStamp current,
            ContentStamp expected
        ) noexcept;
        [[nodiscard]] SessionResult<void> rebind(
            BindingChangePermit& permit,
            SourceBinding binding,
            editing::StateId current,
            PublicationOrder order
        ) noexcept;

    private:
        SessionId id_;
        SourceBinding binding_;
        BindingRevision binding_revision_;
        ObservationVersion observed_;
        PersistenceCheckpoint checkpoint_;
        EditGate gate_;
    };
}
