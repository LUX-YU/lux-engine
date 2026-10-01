#pragma once
#include <lux/engine/editor/sessions/SessionActions.hpp>
#include <lux/engine/editor/persistence/SaveService.hpp>
namespace lux::editor::sessions
{
    namespace detail
    {
        struct SessionInstallationData;
    }
    class PreparedSessionInstallation;
    // Only role owners/tokens. The author object belongs exclusively to SessionStore.
    // Destroying this role bundle revokes roles, not content. close() explicitly closes content.
    class InstalledSession final
    {
    public:
        ~InstalledSession();
        InstalledSession(InstalledSession&&) noexcept;
        InstalledSession& operator=(InstalledSession&&) noexcept;
        InstalledSession(const InstalledSession&) = delete;
        InstalledSession& operator=(const InstalledSession&) = delete;
        [[nodiscard]] SessionId id() const noexcept;
        [[nodiscard]] SessionFactoryResult<HistoryActionsInfo> queryHistory() const;
        [[nodiscard]] SessionFactoryResult<ContentStamp> undo();
        [[nodiscard]] SessionFactoryResult<ContentStamp> redo();
        [[nodiscard]] SessionResult<void> close(ContentStamp);

    private:
        friend class PreparedSessionInstallation;
        explicit InstalledSession(std::shared_ptr<detail::SessionInstallationData>) noexcept;
        std::shared_ptr<detail::SessionInstallationData> data_;
    };
    class PreparedSessionInstallation final
    {
    public:
        // Called after Store.prepare; its moved reservation owns hidden-slot rollback.
        [[nodiscard]] static SessionFactoryResult<PreparedSessionInstallation>
        prepare(
            SessionStore&,
            persistence::SaveService&,
            SessionReservation,
            contracts::CodeLease,
            std::unique_ptr<HistoryActions>,
            std::unique_ptr<persistence::ISaveSource>
        );
        ~PreparedSessionInstallation();
        PreparedSessionInstallation(PreparedSessionInstallation&&) noexcept;
        PreparedSessionInstallation& operator=(PreparedSessionInstallation&&) noexcept;
        PreparedSessionInstallation(const PreparedSessionInstallation&) = delete;
        PreparedSessionInstallation& operator=(const PreparedSessionInstallation&) = delete;
        [[nodiscard]] SessionId id() const noexcept;
        // No notifications here. The caller reports installation before attempting its fact notification.
        [[nodiscard]] SessionFactoryResult<InstalledSession> publish();
        [[nodiscard]] bool usesCode(const contracts::CodeLease&) const noexcept;

    private:
        explicit PreparedSessionInstallation(std::shared_ptr<detail::SessionInstallationData>) noexcept;
        std::shared_ptr<detail::SessionInstallationData> data_;
    };
}
