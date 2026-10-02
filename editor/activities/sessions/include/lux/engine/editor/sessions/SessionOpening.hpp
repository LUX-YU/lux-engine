#pragma once
#include <lux/engine/editor/sessions/SessionFactory.hpp>

namespace lux::process
{
    class ExecutionRuntime;
}
namespace lux::editor::sessions
{
    struct OpenAssetId final
    {
        std::uint64_t value{};
        friend bool operator==(OpenAssetId, OpenAssetId) noexcept = default;
    };
    struct OpenAssetRequest final
    {
        std::uint64_t project_instance{};
        SessionKindId kind;
        SessionLoadInput input;
        // Zero selects the normal shared working copy. A nonzero key is an explicit separate-copy intent.
        std::uint64_t working_copy{};
    };
    enum class EOpenAssetStage : std::uint8_t
    {
        READING,
        PREPARING,
        PUBLISHED,
        FAILED,
        CANCELLED
    };
    struct OpenAssetStatus final
    {
        EOpenAssetStage stage{EOpenAssetStage::READING};
        SessionId session;
        bool reused{};
        bool cancellation_requested{};
        std::optional<SessionFactoryFailure> failure;
    };
    // Concrete content loading and installation owner, independent of any view. Each caller receives
    // a separate waiter; shared read/decode/installation is cancelled only after its last waiter cancels.
    class SessionOpening final
    {
    public:
        SessionOpening(process::ExecutionRuntime&, SessionStore&, persistence::SaveService&, std::size_t capacity = 64);
        ~SessionOpening();
        SessionOpening(const SessionOpening&) = delete;
        SessionOpening& operator=(const SessionOpening&) = delete;
        SessionOpening(SessionOpening&&) = delete;
        SessionOpening& operator=(SessionOpening&&) = delete;
        [[nodiscard]] SessionFactoryResult<OpenAssetId> open(OpenAssetRequest, const SessionFactorySnapshot&);
        // New in-memory content uses the same installation and role owner; no encode/read/decode round trip.
        [[nodiscard]] SessionFactoryResult<OpenAssetId> create(
            std::uint64_t project_instance, SessionPreparation, const SessionFactorySnapshot&
        );
        [[nodiscard]] SessionFactoryResult<OpenAssetStatus> status(OpenAssetId) const;
        [[nodiscard]] SessionFactoryResult<void> cancel(OpenAssetId);
        [[nodiscard]] SessionFactoryResult<void> acknowledge(OpenAssetId);
        // Completion collection only stores owning facts. Installation occurs here, at the owner's safe point.
        [[nodiscard]] SessionFactoryResult<void> update();
        [[nodiscard]] InstalledSession* find(SessionId) noexcept;
        // Uses the catalog fixed at content admission, including after contributions are replaced.
        [[nodiscard]] SessionFactoryResult<std::shared_ptr<SessionFactoryEntry>> factory(SessionId) const;
        // Closes new admission and requests outstanding reads to stop; already published sessions remain.
        void requestStop() noexcept;
        [[nodiscard]] bool settled() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
