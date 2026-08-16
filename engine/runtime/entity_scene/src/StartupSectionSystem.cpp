#include <lux/engine/runtime/entity_scene/StartupSectionSystem.hpp>

#include <lux/engine/resource/entity_scene/EntitySceneCodec.hpp>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace lux::runtime::entity_scene
{
    namespace
    {
        [[nodiscard]] bool sectionIdLess(
            const lux::entity_scene::EntitySectionId& lhs,
            const lux::entity_scene::EntitySectionId& rhs) noexcept
        {
            const auto left = lhs.value().as_bytes();
            const auto right = rhs.value().as_bytes();
            return std::lexicographical_compare(
                left.begin(), left.end(), right.begin(), right.end());
        }

        [[nodiscard]] EEntitySceneError sceneError(
            EEntitySectionRequestError error) noexcept
        {
            switch (error)
            {
            case EEntitySectionRequestError::REQUIREMENT_UNAVAILABLE:
                return EEntitySceneError::REQUIREMENT_UNAVAILABLE;
            case EEntitySectionRequestError::MISSING_DEPENDENCY:
                return EEntitySceneError::DEPENDENCY_UNAVAILABLE;
            case EEntitySectionRequestError::SOURCE_UNAVAILABLE:
                return EEntitySceneError::SOURCE_UNAVAILABLE;
            case EEntitySectionRequestError::OWNER_CLOSED:
            case EEntitySectionRequestError::OWNER_NOT_ADDED:
                return EEntitySceneError::LOADER_UNAVAILABLE;
            default:
                return EEntitySceneError::STARTUP_REQUEST_REJECTED;
            }
        }
    }

    lux::cxx::expected<std::unique_ptr<StartupSectionSystem>, EntitySceneFailure>
    StartupSectionSystem::create(
        const EntitySceneCatalog& catalog,
        EntitySectionLoaderSystem& loader,
        lux::exec::AsyncRuntime& runtime) noexcept
    {
        const auto& manifest = catalog.manifest();

        const auto findSection = [&manifest](
            const lux::entity_scene::EntitySectionId& id)
        {
            return std::lower_bound(
                manifest.sections.begin(),
                manifest.sections.end(),
                id,
                [](const lux::entity_scene::EntitySectionRecord& record,
                   const lux::entity_scene::EntitySectionId& target)
                {
                    return sectionIdLess(record.id, target);
                });
        };

        // Deterministic iterative DFS over canonical startup/dependency lists.
        // Postorder is a topological acquire order: every dependency is
        // admitted before the Section that pins it.
        std::vector<const lux::entity_scene::EntitySectionRecord*> startup;
        std::vector<std::uint8_t> state(manifest.sections.size(), 0u);
        std::vector<std::pair<std::size_t, std::size_t>> stack;
        for (const auto& id : manifest.startup_sections)
        {
            const auto found = findSection(id);
            // validateEntitySceneManifest() already proved this. Keeping the
            // branch makes the factory fail closed if that contract drifts.
            if (found == manifest.sections.end() || found->id != id)
            {
                return lux::cxx::unexpected(EntitySceneFailure{
                    EEntitySceneError::INVALID_MANIFEST,
                    id,
                    {},
                    "startup Section is absent from the validated manifest"});
            }
            const auto root = static_cast<std::size_t>(
                found - manifest.sections.begin());
            if (state[root] == 2u)
                continue;
            stack.emplace_back(root, 0u);
            state[root] = 1u;
            while (!stack.empty())
            {
                auto& [section_index, dependency_index] = stack.back();
                const auto& record = manifest.sections[section_index];
                if (dependency_index < record.dependencies.size())
                {
                    const auto dependency =
                        findSection(record.dependencies[dependency_index++]);
                    if (dependency == manifest.sections.end() ||
                        dependency->id !=
                            record.dependencies[dependency_index - 1u])
                    {
                        return lux::cxx::unexpected(EntitySceneFailure{
                            EEntitySceneError::INVALID_MANIFEST,
                            record.id,
                            {},
                            "dependency is absent from the validated manifest"});
                    }
                    const auto dependency_slot = static_cast<std::size_t>(
                        dependency - manifest.sections.begin());
                    if (state[dependency_slot] == 1u)
                    {
                        return lux::cxx::unexpected(EntitySceneFailure{
                            EEntitySceneError::INVALID_MANIFEST,
                            dependency->id,
                            {},
                            "dependency cycle escaped manifest validation"});
                    }
                    if (state[dependency_slot] == 0u)
                    {
                        state[dependency_slot] = 1u;
                        stack.emplace_back(dependency_slot, 0u);
                    }
                    continue;
                }
                state[section_index] = 2u;
                startup.push_back(&record);
                stack.pop_back();
            }
        }

        std::vector<EntitySectionTicket> tickets;
        std::vector<ReleasedGeneration> released;
        tickets.reserve(startup.size());
        released.reserve(startup.size());
        return std::unique_ptr<StartupSectionSystem>{new StartupSectionSystem(
            catalog,
            loader,
            runtime,
            std::move(startup),
            std::move(tickets),
            std::move(released))};
    }
    StartupSectionSystem::StartupSectionSystem(
        const EntitySceneCatalog& catalog,
        EntitySectionLoaderSystem& loader,
        lux::exec::AsyncRuntime& runtime,
        std::vector<const lux::entity_scene::EntitySectionRecord*> startup,
        std::vector<EntitySectionTicket> tickets,
        std::vector<ReleasedGeneration> released) noexcept
        : catalog_(&catalog),
          client_(loader.client()),
          startup_(std::move(startup)),
          tickets_(std::move(tickets)),
          released_(std::move(released)),
          close_scope_(runtime),
          close_admission_(close_scope_.tryAcquireAdmission())
    {
        if (!close_admission_)
            std::abort();
    }

    StartupSectionSystem::~StartupSectionSystem()
    {
        // An unpublished ScheduleBuilder candidate may be rolled back after
        // this system was constructed but before onAdded(). No tickets or
        // registry state can exist in that case, so close locally instead of
        // turning an ordinary assembly rejection into a process abort.
        if (!added_ && state_ != EEntitySceneState::CLOSED)
            requestClose();
        if (state_ != EEntitySceneState::CLOSED)
            std::abort();
    }

    void StartupSectionSystem::onAdded(
        const lux::ecs::SystemSetupContext& setup)
    {
        added_ = true;
        if (!client_.boundTo(setup.registry()))
        {
            fail(
                EEntitySceneError::LOADER_UNAVAILABLE,
                "EntityScene loader is not bound to this registry");
        }
    }

    void StartupSectionSystem::update(
        const lux::ecs::SystemUpdateContext&)
    {
        if (state_ == EEntitySceneState::CLOSING)
        {
            tryCompleteClose();
            return;
        }
        if (state_ != EEntitySceneState::LOADING)
            return;
        if (!added_ || !client_)
        {
            fail(
                EEntitySceneError::LOADER_UNAVAILABLE,
                "EntityScene loader admission is unavailable");
            return;
        }

        if (!preflighted_)
        {
            const auto manifest_requirements = client_.validateRequirements(
                catalog_->manifest().required_extensions,
                catalog_->manifest().required_components);
            if (!manifest_requirements)
            {
                fail(
                    sceneError(manifest_requirements.error()),
                    "EntityScene manifest requirement is unavailable",
                    {},
                    manifest_requirements.error());
                return;
            }
            for (const auto* section : startup_)
            {
                const auto valid = client_.validate(*section);
                if (!valid)
                {
                    fail(
                        sceneError(valid.error()),
                        "startup EntitySection failed admission preflight",
                        section->id,
                        valid.error());
                    return;
                }
            }
            preflighted_ = true;
        }

        if (!acquired_)
        {
            for (const auto* section : startup_)
            {
                auto ticket = client_.acquire(*section);
                if (!ticket)
                {
                    fail(
                        sceneError(ticket.error()),
                        "startup EntitySection request was rejected",
                        section->id,
                        ticket.error());
                    return;
                }
                tickets_.push_back(std::move(*ticket));
            }
            acquired_ = true;
        }

        std::size_t active = 0u;
        for (std::size_t index = 0u; index < tickets_.size(); ++index)
        {
            switch (tickets_[index].state())
            {
            case EEntitySectionState::ACTIVE:
                ++active;
                break;
            case EEntitySectionState::FAILED:
                fail(
                    EEntitySceneError::STARTUP_SECTION_FAILED,
                    "startup EntitySection failed before Scene publication",
                    startup_[index]->id);
                return;
            default:
                break;
            }
        }
        if (active == tickets_.size())
        {
            state_ = EEntitySceneState::READY;
            ++revision_;
        }
    }

    std::span<const lux::ecs::ISystem::Type>
    StartupSectionSystem::prerequisites() const noexcept
    {
        static constexpr Type prerequisites[]{
            lux::ecs::systemType<EntitySectionLoaderSystem>()};
        return prerequisites;
    }

    std::span<const lux::ecs::ISystem::Type>
    StartupSectionSystem::runsAfter() const noexcept
    {
        return prerequisites();
    }

    void StartupSectionSystem::requestClose() noexcept
    {
        requestClose({});
    }

    void StartupSectionSystem::requestClose(
        lux::ecs::SystemCloseProgressSink progress) noexcept
    {
        if (progress)
            close_progress_ = progress;
        if (state_ != EEntitySceneState::CLOSING &&
            state_ != EEntitySceneState::CLOSED)
        {
            state_ = EEntitySceneState::CLOSING;
            releaseTicketsReverse();
            close_scope_.requestStop();
            tryCompleteClose();
        }
        if (!close_scope_subscribed_)
        {
            close_scope_subscribed_ = true;
            lux::exec::detail::subscribeScopeClose(
                close_scope_,
                [this]() noexcept { acceptCloseScopeClosed(); });
        }
    }

    bool StartupSectionSystem::closeComplete() const noexcept
    {
        return state_ == EEntitySceneState::CLOSED && close_scope_closed_;
    }

    bool StartupSectionSystem::closeNeedsOwnerTick() const noexcept
    {
        return state_ == EEntitySceneState::CLOSING;
    }

    lux::exec::AsyncScopeCloseSender StartupSectionSystem::closeAsync() noexcept
    {
        requestClose();
        return close_scope_.closeAsync();
    }

    EEntitySceneState StartupSectionSystem::state() const noexcept
    {
        return state_;
    }

    std::uint64_t StartupSectionSystem::revision() const noexcept
    {
        return revision_;
    }

    EntitySceneSnapshot StartupSectionSystem::snapshot() const noexcept
    {
        EntitySceneSnapshot result;
        result.state = state_;
        result.revision = revision_;
        result.startup_sections = startup_.size();
        result.contributions = catalog_->manifest().contributions.size();
        for (const auto& ticket : tickets_)
        {
            const auto ticket_state = ticket.state();
            if (ticket_state == EEntitySectionState::ACTIVE)
                ++result.active_startup_sections;
            else if (ticket_state == EEntitySectionState::FAILED)
                ++result.failed_startup_sections;
        }
        return result;
    }

    const std::optional<EntitySceneFailure>& StartupSectionSystem::failure()
        const noexcept
    {
        return failure_;
    }

    const lux::entity_scene::EntitySceneId& StartupSectionSystem::sceneId()
        const noexcept
    {
        return catalog_->manifest().id;
    }

    std::span<const lux::entity_scene::SceneContribution>
    StartupSectionSystem::contributions() const noexcept
    {
        return catalog_->manifest().contributions;
    }

    void StartupSectionSystem::fail(
        EEntitySceneError error,
        std::string detail,
        lux::entity_scene::EntitySectionId section,
        std::optional<EEntitySectionRequestError> request_error) noexcept
    {
        if (state_ != EEntitySceneState::LOADING)
            return;
        failure_ = EntitySceneFailure{
            error, section, request_error, std::move(detail)};
        state_ = EEntitySceneState::FAILED;
        releaseTicketsReverse();
    }

    bool StartupSectionSystem::releasesSettled() const noexcept
    {
        return std::all_of(
            released_.begin(),
            released_.end(),
            [this](const ReleasedGeneration& released)
            {
                return client_.releaseSettled(
                    released.section, released.generation);
            });
    }

    void StartupSectionSystem::acceptCloseScopeClosed() noexcept
    {
        close_scope_closed_ = true;
        if (close_progress_)
            close_progress_.notify();
    }

    void StartupSectionSystem::tryCompleteClose() noexcept
    {
        if (state_ != EEntitySceneState::CLOSING || !releasesSettled())
            return;
        client_ = {};
        released_.clear();
        close_admission_ = {};
        state_ = EEntitySceneState::CLOSED;
    }

    void StartupSectionSystem::releaseTicketsReverse() noexcept
    {
        while (!tickets_.empty())
        {
            const auto index = tickets_.size() - 1u;
            released_.push_back(ReleasedGeneration{
                startup_[index]->id, tickets_.back().generation()});
            tickets_.back().reset();
            tickets_.pop_back();
        }
    }
}
