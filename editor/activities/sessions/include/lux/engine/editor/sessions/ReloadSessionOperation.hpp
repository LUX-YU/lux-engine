#pragma once
#include <lux/engine/editor/sessions/SessionFactory.hpp>
namespace lux::process
{
    class ExecutionRuntime;
}
namespace lux::editor::sessions
{
    // One reviewed source read. It never owns/binds a live Session on the worker and never holds READING
    // across frames. The physical target observation catches intervening writes, including acknowledged ones.
    class ReloadSessionOperation final
    {
    public:
        [[nodiscard]] static SessionFactoryResult<std::unique_ptr<ReloadSessionOperation>> start(
            process::ExecutionRuntime&,
            SessionStore&,
            persistence::WriteCoordinator&,
            std::shared_ptr<SessionFactoryEntry>,
            SessionLoadInput
        );
        ~ReloadSessionOperation();
        ReloadSessionOperation(const ReloadSessionOperation&) = delete;
        ReloadSessionOperation& operator=(const ReloadSessionOperation&) = delete;
        ReloadSessionOperation(ReloadSessionOperation&&) = delete;
        ReloadSessionOperation& operator=(ReloadSessionOperation&&) = delete;
        // Borrow the role only for this call. A closed/replaced installation is never retained across frames.
        void update(InstalledSession&);
        void cancel() noexcept;
        [[nodiscard]] const std::optional<SessionFactoryResult<ContentStamp>>& outcome() const noexcept;

    private:
        struct Impl;
        explicit ReloadSessionOperation(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
