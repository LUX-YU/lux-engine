#pragma once
#include <lux/engine/editor/sessions/SessionInstallation.hpp>
#include <lux/cxx/core/function_ref.hpp>
#include <span>

namespace lux::editor::sessions
{
    struct SaveAllEntry final
    {
        SessionId session;
        bool already_clean{};
        std::optional<persistence::SaveId> save;
        std::optional<SessionFactoryFailure> failure;
    };
    // An owning fixed set and the original SaveIds, never a parallel save state machine.
    // The caller retains this report until its accepted SaveIds have been settled and acknowledged.
    class SaveAllOperation final
    {
    public:
        [[nodiscard]] static SessionFactoryResult<SaveAllOperation> begin(SessionStore&, persistence::SaveService&);
        // Synchronously admit each dirty member of the fixed set through its actual save owner.
        // The callback is borrowed only for begin(); successful IDs remain owned by that receiver.
        using Request = cxx::function_ref<SessionFactoryResult<persistence::SaveId>(ContentStamp)>;
        [[nodiscard]] static SessionFactoryResult<SaveAllOperation> begin(SessionStore&, Request);
        SaveAllOperation(const SaveAllOperation&) = delete;
        SaveAllOperation& operator=(const SaveAllOperation&) = delete;
        SaveAllOperation(SaveAllOperation&&) noexcept = default;
        SaveAllOperation& operator=(SaveAllOperation&&) noexcept = default;
        [[nodiscard]] std::span<const SaveAllEntry> entries() const noexcept
        {
            return entries_;
        }

    private:
        SaveAllOperation() = default;
        std::vector<SaveAllEntry> entries_;
    };
    enum class ECloseChoice : std::uint8_t
    {
        SAVE,
        DISCARD,
        CANCEL
    };
    struct SessionCloseDecision final
    {
        ContentStamp content;
        ECloseChoice choice{ECloseChoice::CANCEL};
        std::optional<persistence::WriteTarget> destination;
        asset::AssetId destination_asset;
    };
    // UI/run dependency preparation is composed by the application. No content is removed here.
    // Once prepare() returns every permit, the original Store commits the whole set in one call.
    class CloseSessionsOperation final
    {
    public:
        [[nodiscard]] static SessionFactoryResult<CloseSessionsOperation>
        begin(SessionStore&, persistence::SaveService&, std::vector<SessionCloseDecision>);
        CloseSessionsOperation(const CloseSessionsOperation&) = delete;
        CloseSessionsOperation& operator=(const CloseSessionsOperation&) = delete;
        CloseSessionsOperation(CloseSessionsOperation&&) noexcept = default;
        CloseSessionsOperation& operator=(CloseSessionsOperation&&) = delete;
        [[nodiscard]] SessionFactoryResult<std::vector<ClosePermit>> prepare();
        [[nodiscard]] std::span<const SaveAllEntry> saves() const noexcept
        {
            return saves_;
        }

    private:
        CloseSessionsOperation(SessionStore&, persistence::SaveService&, std::vector<SessionCloseDecision>);
        SessionStore& store_;
        persistence::SaveService& service_;
        std::vector<SessionCloseDecision> decisions_;
        std::vector<SaveAllEntry> saves_;
    };
    [[nodiscard]] SessionFactoryFailure factoryFailure(const persistence::PersistenceFailure&);
}
