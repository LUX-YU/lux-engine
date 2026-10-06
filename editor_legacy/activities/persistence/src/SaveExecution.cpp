#include <lux/engine/editor/persistence/SaveExecution.hpp>

namespace lux::editor::persistence
{
    using namespace persistence;
    namespace
    {
        void receiveEncoding(SaveService& service, SaveId id, PersistenceResult<EncodedArtifact> result) noexcept
        {
            // TaskScope delivers every admitted job once on the owner; ENCODING cannot be acknowledged.
            // Role dispatch is supported by completeEncoding, so no valid result needs buffering/retry.
            // Rejection here means an invalid adapter/service lifetime or duplicate delivery, not BUSY
            // backpressure. Keep that contract failure visible in release builds too.
            if (!service.completeEncoding(id, std::move(result)))
                std::terminate();
        }
        // Foreign store code may fail after changing disk. An exception cannot prove NotPublished.
        VPublicationOutcome publish(IArtifactStore& store, const PublicationQuery& work, std::stop_token stop) noexcept
        try
        {
            return store.publish(work, stop);
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return PublicationUnknown{{EPersistenceError::IO, "Store callback failed"}, "foreign-publish"};
        }
    }
    SaveExecution::SaveExecution(
        process::ExecutionRuntime& runtime,
        SaveService& service,
        WriteCoordinator& coordinator,
        IArtifactStore& store
    )
        : tasks_(runtime), service_(service), coordinator_(coordinator), store_(store)
    {}
    SaveExecution::~SaveExecution()
    {
        tasks_.requestStop();
        (void)tasks_.join();
    }
    SaveExecution::SaveExecution(
        process::ExecutionRuntime& runtime,
        std::shared_ptr<SaveService> service,
        std::shared_ptr<WriteCoordinator> coordinator,
        std::shared_ptr<IArtifactStore> store
    )
        : service_owner_(std::move(service)), coordinator_owner_(std::move(coordinator)),
          store_owner_(std::move(store)), tasks_(runtime), service_(*service_owner_), coordinator_(*coordinator_owner_),
          store_(*store_owner_)
    {
    }
    PersistenceResult<void> SaveExecution::submitReady()
    {
        for (;;)
        {
            auto ready = service_.takeEncoding();
            if (!ready)
                return lux::cxx::unexpected(ready.error());
            if (!*ready)
                break;
            auto work = std::move(**ready);
            const auto id = work.id;
            auto submitted = tasks_.submit(
                {.name = "Encode author source"},
                [cpu = tasks_.execution().cpu(),
                 work = std::move(work)](process::TaskReporter reporter) mutable noexcept {
                    return stdexec::then(
                        stdexec::schedule(cpu),
                        [work = std::move(work), stop = reporter.stopToken()]() mutable {
                            return work.encoding.encode(stop);
                        }
                    );
                },
                [this, id](process::TTaskResult<EncodedArtifact, PersistenceFailure>&& result) noexcept {
                    if (result)
                        receiveEncoding(service_, id, std::move(*result));
                    else if (auto* error = result.error().domainFailure())
                        receiveEncoding(service_, id, lux::cxx::unexpected(std::move(*error)));
                    else
                        receiveEncoding(
                            service_,
                            id,
                            lux::cxx::unexpected(PersistenceFailure{
                                result.error().isCancelled() ? EPersistenceError::CANCELLED
                                                             : EPersistenceError::EXECUTION
                            })
                        );
                }
            );
            if (!submitted)
                receiveEncoding(service_, id, lux::cxx::unexpected(PersistenceFailure{EPersistenceError::EXECUTION}));
        }
        auto blocking = tasks_.execution().blocking();
        if (!blocking)
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::EXECUTION});
        for (;;)
        {
            auto ready = coordinator_.takeReady();
            if (!ready)
                return lux::cxx::unexpected(ready.error());
            if (!*ready)
                break;
            auto work = std::move(**ready);
            const auto ticket = work.ticket;
            auto submitted = tasks_.submit(
                {.name = "Publish author source"},
                [scheduler = *blocking, store = &store_, work = std::move(work)](process::TaskReporter reporter
                ) mutable noexcept {
                    return stdexec::then(
                        stdexec::schedule(scheduler),
                        [store, work = std::move(work), stop = reporter.stopToken()](
                        ) -> PersistenceResult<VPublicationOutcome> { return publish(*store, work, stop); }
                    );
                },
                [this, ticket](process::TTaskResult<VPublicationOutcome, PersistenceFailure>&& result) noexcept {
                    if (result)
                        (void)coordinator_.complete(ticket, std::move(*result));
                    else if (result.error().isCancelled() || result.error().executionFailure())
                        (void)coordinator_.complete(ticket, NotPublished{{EPersistenceError::EXECUTION}});
                    else
                        (void)coordinator_.complete(ticket, PublicationUnknown{{EPersistenceError::IO}, "execution"});
                }
            );
            if (!submitted)
                (void)coordinator_.complete(ticket, NotPublished{{EPersistenceError::EXECUTION}});
        }
        return {};
    }
}
