#include <lux/engine/editor/sessions/SessionOpening.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <algorithm>
#include <thread>

namespace lux::editor::sessions
{
    struct SessionOpening::Impl final
    {
        struct Key final
        {
            std::uint64_t project{};
            SessionKindId kind;
            std::string location;
            asset::AssetId asset;
            std::uint64_t copy{};
            friend bool operator==(const Key&, const Key&) noexcept = default;
        };
        struct Work final
        {
            Key key;
            SessionFactorySnapshot factories;
            process::TaskId task;
            std::optional<SessionFactoryResult<SessionPreparation>> result;
            std::optional<PreparedSessionInstallation> prepared;
            OpenAssetStatus status;
        };
        struct Waiter final
        {
            OpenAssetId id;
            std::shared_ptr<Work> work;
            bool cancelled{};
        };
        struct Roles final
        {
            Key key;
            SessionFactorySnapshot factories;
            InstalledSession session;
        };
        struct Dispatch final
        {
            bool& active;
            explicit Dispatch(bool& flag) noexcept : active(flag)
            {
                active = true;
            }
            ~Dispatch()
            {
                active = false;
            }
            Dispatch(const Dispatch&) = delete;
        };
        const std::thread::id owner{std::this_thread::get_id()};
        SessionStore& store;
        persistence::SaveService& saves;
        services::ServiceRegistry& services;
        services::ServiceScope& scope;
        std::size_t capacity;
        std::uint64_t next{1};
        bool dispatching{};
        bool stopping{};
        std::vector<Waiter> waiters;
        std::vector<std::shared_ptr<Work>> works;
        std::vector<Roles> roles;
        process::TaskScope tasks;
        Impl(
            process::ExecutionRuntime& runtime,
            SessionStore& content,
            persistence::SaveService& saving,
            services::ServiceRegistry& dependencies,
            services::ServiceScope& lifetime,
            std::size_t limit
        )
            : store(content), saves(saving), services(dependencies), scope(lifetime), capacity(limit), tasks(runtime)
        {
            waiters.reserve(limit);
            works.reserve(limit);
            roles.reserve(limit);
        }
        ~Impl()
        {
            tasks.requestStop();
            (void)tasks.join(); // Result owners still exist while completion callbacks run.
        }
        SessionFactoryResult<void> enter(bool business = true) const
        {
            if (owner != std::this_thread::get_id())
                return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::WRONG_THREAD, "open"});
            if (business && dispatching)
                return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::BUSY, "open"});
            return {};
        }
        auto waiter(OpenAssetId id)
        {
            return std::ranges::find(waiters, id, &Waiter::id);
        }
        bool wanted(const std::shared_ptr<Work>& work) const
        {
            return std::ranges::any_of(
                waiters,
                [&](const auto& waiter) { return waiter.work == work && !waiter.cancelled; }
            );
        }
        void fail(Work& work, SessionFactoryFailure failure)
        {
            work.status.stage =
                failure.code == ESessionFactoryError::CANCELLED ? EOpenAssetStage::CANCELLED : EOpenAssetStage::FAILED;
            work.status.failure = std::move(failure);
            work.prepared.reset();
            work.result.reset();
        }
    };
    SessionOpening::SessionOpening(
        process::ExecutionRuntime& runtime,
        SessionStore& store,
        persistence::SaveService& saves,
        services::ServiceRegistry& services,
        services::ServiceScope& scope,
        std::size_t capacity
    )
        : impl_(std::make_unique<Impl>(runtime, store, saves, services, scope, capacity))
    {
    }
    SessionOpening::~SessionOpening() = default;
    SessionFactoryResult<OpenAssetId> SessionOpening::create(
        std::uint64_t project_instance,
        SessionPreparation input,
        const SessionFactorySnapshot& factories
    )
    {
        if (auto entered = impl_->enter(); !entered)
            return cxx::unexpected(entered.error());
        Impl::Dispatch dispatch{impl_->dispatching};
        auto owned = std::move(input);
        if (impl_->stopping)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CLOSED, "create"});
        if (!project_instance)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "create"});
        const bool capacity = impl_->waiters.size() == impl_->capacity || impl_->works.size() == impl_->capacity ||
                              impl_->roles.size() == impl_->capacity || impl_->next == UINT64_MAX;
        if (capacity)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CAPACITY, "create"});
        const OpenAssetId id{impl_->next++};
        auto work = std::make_shared<Impl::Work>();
        work->key = {project_instance, {}, {}, {}, 0};
        work->factories = factories;
        work->status.stage = EOpenAssetStage::PREPARING;
        work->result.emplace(std::move(owned));
        impl_->works.push_back(work);
        impl_->waiters.push_back({id, std::move(work)});
        return id;
    }
    SessionFactoryResult<OpenAssetId> SessionOpening::open(
        OpenAssetRequest input,
        const SessionFactorySnapshot& factories
    )
    {
        if (auto entered = impl_->enter(); !entered)
            return cxx::unexpected(entered.error());
        Impl::Dispatch dispatch{impl_->dispatching};
        // Admitted rejected payloads and their code pins are released before dispatch is restored.
        auto request = std::move(input);
        if (impl_->stopping)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CLOSED, "open"});
        const bool invalid = !request.project_instance || !request.input.binding || request.input.reload ||
                             request.input.asset.isNull() || !request.input.source ||
                             request.input.binding->asset != request.input.asset;
        if (invalid)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "open.request"});
        if (impl_->waiters.size() == impl_->capacity || impl_->next == UINT64_MAX)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CAPACITY, "open.waiters"});
        auto factory = factories.find(request.kind);
        if (!factory)
            return cxx::unexpected(factory.error());
        Impl::Key key{
            request.project_instance,
            request.kind,
            request.input.target ? request.input.target->key.value : request.input.binding->location,
            request.input.asset,
            request.working_copy
        };
        const OpenAssetId id{impl_->next++};
        for (const auto& work : impl_->works)
        {
            const bool active =
                work->status.stage == EOpenAssetStage::READING || work->status.stage == EOpenAssetStage::PREPARING;
            if (active && work->key == key && impl_->wanted(work))
            {
                impl_->waiters.push_back({id, work});
                return id;
            }
        }
        auto work = std::make_shared<Impl::Work>();
        work->key = key;
        work->factories = factories;
        for (const auto& role : impl_->roles)
        {
            // InstalledSession::close consumes its role bundle. The completed close has no
            // Store identity left to query, including before the next maintenance pass.
            if (!role.session.id().valid())
                continue;
            const bool same_domain = role.key.project == key.project && role.key.copy == key.copy;
            if (!same_domain)
                continue;
            auto current = impl_->store.describe(role.session.id());
            if (!current)
            {
                if (current.error() == ESessionError::STALE_SESSION)
                    continue;
                return cxx::unexpected(factoryFailure(current.error()));
            }
            if (current->kind == key.kind && current->binding && current->binding->asset == key.asset &&
                (current->binding->location == key.location ||
                 (role.key == key && current->binding == request.input.binding)))
            {
                work->status = {EOpenAssetStage::PUBLISHED, role.session.id(), true};
                impl_->waiters.push_back({id, std::move(work)});
                return id;
            }
        }
        if (impl_->works.size() == impl_->capacity || impl_->roles.size() == impl_->capacity)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CAPACITY, "open.content"});
        auto scheduler = impl_->tasks.execution().blocking();
        if (!scheduler)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::IO, "execution.blocking"});
        auto job = SessionLoadJob::prepare(*factory, std::move(request.input), impl_->services, impl_->scope);
        if (!job)
        {
            return cxx::unexpected(std::move(job.error()));
        }
        if (impl_->stopping)
        {
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CLOSED, "open.prepare"});
        }
        impl_->works.push_back(work);
        impl_->waiters.push_back({id, work});
        auto submitted = impl_->tasks.submit(
            {.name = "Open asset source"},
            [scheduler = *scheduler, job = std::move(*job)](process::TaskReporter reporter) mutable noexcept
            {
                return stdexec::then(
                    stdexec::schedule(scheduler),
                    [job = std::move(job), stop = reporter.stopToken()]() mutable { return std::move(job).run(stop); }
                );
            },
            [work](process::TTaskResult<SessionPreparation, SessionFactoryFailure>&& completed) noexcept
            {
                // Accepted facts are received regardless of business dispatch/admission; never call a factory here.
                if (completed)
                    work->result.emplace(std::move(*completed));
                else if (auto* failure = completed.error().domainFailure())
                    work->result.emplace(cxx::unexpected(std::move(*failure)));
                else
                    work->result.emplace(cxx::unexpected(SessionFactoryFailure{
                        completed.error().isCancelled() ? ESessionFactoryError::CANCELLED : ESessionFactoryError::IO,
                        "execution.open"
                    }));
            }
        );
        if (submitted)
            work->task = *submitted;
        else
            impl_->fail(
                *work,
                {ESessionFactoryError::IO, "execution.open", static_cast<std::uint64_t>(submitted.error())}
            );
        return id;
    }
    SessionFactoryResult<OpenAssetStatus> SessionOpening::status(OpenAssetId id) const
    {
        if (auto entered = impl_->enter(false); !entered)
            return cxx::unexpected(entered.error());
        const auto waiter = impl_->waiter(id);
        if (waiter == impl_->waiters.end())
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::NOT_FOUND, "open.id"});
        auto status = waiter->work->status;
        status.cancellation_requested = waiter->cancelled;
        if (waiter->cancelled && status.stage != EOpenAssetStage::PUBLISHED)
            status.stage = EOpenAssetStage::CANCELLED;
        return status;
    }
    SessionFactoryResult<void> SessionOpening::cancel(OpenAssetId id)
    {
        if (auto entered = impl_->enter(); !entered)
            return entered;
        const auto waiter = impl_->waiter(id);
        if (waiter == impl_->waiters.end())
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::NOT_FOUND, "open.id"});
        waiter->cancelled = true;
        if (!impl_->wanted(waiter->work) && waiter->work->status.stage != EOpenAssetStage::PUBLISHED)
            (void)impl_->tasks.execution().requestStop(waiter->work->task);
        return {};
    }
    SessionFactoryResult<void> SessionOpening::acknowledge(OpenAssetId id)
    {
        if (auto entered = impl_->enter(); !entered)
            return entered;
        auto current = status(id);
        if (!current)
            return cxx::unexpected(current.error());
        const bool pending = current->stage == EOpenAssetStage::READING || current->stage == EOpenAssetStage::PREPARING;
        if (pending)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::BUSY, "open.pending"});
        Impl::Dispatch dispatch{impl_->dispatching};
        impl_->waiters.erase(impl_->waiter(id));
        return {};
    }
    SessionFactoryResult<void> SessionOpening::update()
    {
        if (auto entered = impl_->enter(); !entered)
            return entered;
        Impl::Dispatch dispatch{impl_->dispatching};
        for (auto it = impl_->roles.begin(); it != impl_->roles.end();)
        {
            if (!it->session.id().valid())
            {
                it = impl_->roles.erase(it);
                continue;
            }
            const auto current = impl_->store.describe(it->session.id());
            if (!current && current.error() == ESessionError::STALE_SESSION)
                it = impl_->roles.erase(it);
            else
                ++it;
        }
        for (const auto& work : impl_->works)
        {
            if (work->status.stage != EOpenAssetStage::READING && work->status.stage != EOpenAssetStage::PREPARING)
                continue;
            if (!work->result)
                continue;
            if (!impl_->wanted(work))
            {
                impl_->fail(*work, {ESessionFactoryError::CANCELLED, "open.cancelled"});
                continue;
            }
            if (!*work->result)
            {
                impl_->fail(*work, std::move(work->result->error()));
                continue;
            }
            work->status.stage = EOpenAssetStage::PREPARING;
            if (impl_->roles.size() == impl_->capacity)
            {
                impl_->fail(*work, {ESessionFactoryError::CAPACITY, "open.roles"});
                continue;
            }
            if (!work->prepared)
            {
                auto prepared = std::move(**work->result).prepare(impl_->store, impl_->saves);
                if (!prepared)
                {
                    if (prepared.error().code != ESessionFactoryError::BUSY)
                        impl_->fail(*work, std::move(prepared.error()));
                    continue;
                }
                work->prepared.emplace(std::move(*prepared));
            }
            if (!impl_->wanted(work))
            {
                impl_->fail(*work, {ESessionFactoryError::CANCELLED, "open.cancelled"});
                continue;
            }
            auto installed_key = work->key; // Allocation precedes the content publication commit.
            auto installed = work->prepared->publish();
            if (!installed)
            {
                if (installed.error().code != ESessionFactoryError::BUSY)
                    impl_->fail(*work, std::move(installed.error()));
                continue;
            }
            work->status = {EOpenAssetStage::PUBLISHED, installed->id()};
            impl_->roles.push_back({std::move(installed_key), work->factories, std::move(*installed)});
            work->prepared.reset();
            work->result.reset();
        }
        std::erase_if(
            impl_->works,
            [&](const auto& work)
            {
                const bool terminal =
                    work->status.stage != EOpenAssetStage::READING && work->status.stage != EOpenAssetStage::PREPARING;
                return terminal &&
                       std::ranges::none_of(impl_->waiters, [&](const auto& waiter) { return waiter.work == work; });
            }
        );
        return {};
    }
    InstalledSession* SessionOpening::find(SessionId id) noexcept
    {
        if (impl_->owner != std::this_thread::get_id() || impl_->dispatching)
            return nullptr;
        for (auto& role : impl_->roles)
            if (role.session.id() == id)
                return &role.session;
        return nullptr;
    }
    SessionFactoryResult<std::shared_ptr<SessionFactoryEntry>> SessionOpening::factory(SessionId id) const
    {
        if (auto entered = impl_->enter(); !entered)
            return cxx::unexpected(entered.error());
        Impl::Dispatch dispatch{impl_->dispatching};
        auto current = impl_->store.describe(id);
        if (!current)
            return cxx::unexpected(factoryFailure(current.error()));
        for (const auto& role : impl_->roles)
            if (role.session.id() == id)
                return role.factories.find(current->kind);
        return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::NOT_FOUND, "content.factory"});
    }
    void SessionOpening::requestStop() noexcept
    {
        if (impl_->owner != std::this_thread::get_id())
            std::terminate();
        impl_->stopping = true;
        for (auto& waiter : impl_->waiters)
            waiter.cancelled = true;
        impl_->tasks.requestStop();
    }
    bool SessionOpening::settled() const noexcept
    {
        return std::ranges::none_of(
            impl_->works,
            [](const auto& work) {
                return work->status.stage == EOpenAssetStage::READING ||
                       work->status.stage == EOpenAssetStage::PREPARING;
            }
        );
    }
} // namespace lux::editor::sessions
