#include <lux/engine/editor/sessions/ReloadSessionOperation.hpp>
#include <lux/engine/editor/sessions/SessionOperations.hpp>
#include <lux/engine/process/TaskScope.hpp>

namespace lux::editor::sessions
{
    struct ReloadSessionOperation::Impl final
    {
        SessionStore& store;
        ContentStamp expected;
        persistence::WriteObservation writes;
        std::optional<SessionFactoryResult<SessionPreparation>> loaded;
        std::optional<PreparedSessionReload> prepared;
        std::optional<SessionFactoryResult<ContentStamp>> result;
        bool cancelled{};
        bool dispatching{};
        process::TaskScope tasks;
        Impl(
            process::ExecutionRuntime& runtime,
            SessionStore& sessions,
            ContentStamp reviewed,
            persistence::WriteObservation watch
        )
            : store(sessions), expected(reviewed), writes(std::move(watch)), tasks(runtime)
        {
        }
        ~Impl()
        {
            tasks.requestStop();
            (void)tasks.join();
        }
    };
    ReloadSessionOperation::ReloadSessionOperation(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    ReloadSessionOperation::~ReloadSessionOperation() = default;
    SessionFactoryResult<std::unique_ptr<ReloadSessionOperation>> ReloadSessionOperation::start(
        process::ExecutionRuntime& runtime,
        SessionStore& store,
        persistence::WriteCoordinator& writes,
        std::shared_ptr<SessionFactoryEntry> factory,
        SessionLoadInput input
    )
    {
        if (!input.reload || !input.target || !input.binding || !factory)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "reload.request"});
        auto current = store.describe(input.reload->session);
        if (!current)
            return cxx::unexpected(factoryFailure(current.error()));
        const bool is_content_stale = current->current != *input.reload || current->binding != input.binding;
        const bool is_identity_mismatch =
            current->kind.name != factory->descriptor().kind.name() || input.asset != input.binding->asset;
        const bool is_stale = is_content_stale || is_identity_mismatch;
        if (is_stale)
            return cxx::unexpected(factoryFailure(ESessionError::STALE_CONTENT));
        auto scheduler = runtime.blocking();
        if (!scheduler)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::IO, "execution.blocking"});
        auto observed = writes.observeIdle(input.target->key);
        if (!observed)
            return cxx::unexpected(factoryFailure(observed.error()));
        auto impl = std::make_unique<Impl>(runtime, store, *input.reload, std::move(*observed));
        auto* receiving = impl.get();
        auto submitted = impl->tasks.submit(
            {.name = "Reload asset source"},
            [scheduler = *scheduler,
             job =
                 SessionLoadJob{std::move(factory), std::move(input)}](process::TaskReporter reporter) mutable noexcept
            {
                return stdexec::then(
                    stdexec::schedule(scheduler),
                    [job = std::move(job), stop = reporter.stopToken()]() mutable { return std::move(job).run(stop); }
                );
            },
            [receiving](process::TTaskResult<SessionPreparation, SessionFactoryFailure>&& result) noexcept
            {
                if (result)
                    receiving->loaded.emplace(std::move(*result));
                else if (auto* error = result.error().domainFailure())
                    receiving->loaded.emplace(cxx::unexpected(std::move(*error)));
                else
                    receiving->loaded.emplace(cxx::unexpected(SessionFactoryFailure{
                        result.error().isCancelled() ? ESessionFactoryError::CANCELLED : ESessionFactoryError::IO,
                        "execution.reload"
                    }));
            }
        );
        if (!submitted)
            return cxx::unexpected(SessionFactoryFailure{
                ESessionFactoryError::IO,
                "execution.reload",
                static_cast<std::uint64_t>(submitted.error())
            });
        return std::unique_ptr<ReloadSessionOperation>(new ReloadSessionOperation(std::move(impl)));
    }
    void ReloadSessionOperation::update(InstalledSession* role)
    {
        if (impl_->result || impl_->dispatching || !impl_->loaded)
            return;
        struct Scope final
        {
            bool& flag;
            explicit Scope(bool& value) : flag(value)
            {
                flag = true;
            }
            ~Scope()
            {
                flag = false;
            }
        } scope{impl_->dispatching};
        const auto fail = [&](SessionFactoryFailure failure)
        {
            impl_->result.emplace(cxx::unexpected(std::move(failure)));
            impl_->prepared.reset();
            impl_->loaded.reset();
        };
        if (impl_->cancelled)
        {
            fail({ESessionFactoryError::CANCELLED, "reload.cancelled"});
            return;
        }
        if (!*impl_->loaded)
        {
            fail(std::move(impl_->loaded->error()));
            return;
        }
        if (!role || role->id() != impl_->expected.session)
        {
            fail(factoryFailure(ESessionError::STALE_SESSION));
            return;
        }
        if (!impl_->prepared)
        {
            auto prepared = std::move(**impl_->loaded).prepareReload(impl_->store);
            if (!prepared)
            {
                if (prepared.error().code != ESessionFactoryError::BUSY)
                    fail(std::move(prepared.error()));
                return;
            }
            impl_->prepared.emplace(std::move(*prepared));
        }
        // Preparation may invoke extensions; cancellation and the original write observation are
        // rechecked afterwards. InstalledSession performs final source validation before the no-callback swap.
        if (impl_->cancelled)
        {
            fail({ESessionFactoryError::CANCELLED, "reload.cancelled"});
            return;
        }
        auto adopted = role->reload(*impl_->prepared, impl_->writes);
        if (!adopted && adopted.error().code == ESessionFactoryError::BUSY)
            return;
        impl_->result.emplace(std::move(adopted));
        impl_->prepared.reset();
        impl_->loaded.reset();
    }
    void ReloadSessionOperation::cancel() noexcept
    {
        impl_->cancelled = true;
        impl_->tasks.requestStop();
    }
    const std::optional<SessionFactoryResult<ContentStamp>>& ReloadSessionOperation::outcome() const noexcept
    {
        return impl_->result;
    }
} // namespace lux::editor::sessions
