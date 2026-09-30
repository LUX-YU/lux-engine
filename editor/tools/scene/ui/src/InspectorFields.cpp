#include <lux/engine/editor/scene/InspectorFields.hpp>
#include <new>

namespace lux::editor::scene
{
    namespace
    {
        auto rejected(ESceneEditError error)
        {
            return cxx::unexpected(SceneEditError{error});
        }
        bool stale(const SceneEditError& error) noexcept
        {
            return error.code == ESceneEditError::SESSION && error.session == sessions::ESessionError::STALE_SESSION;
        }
    }
    InspectorFields::InspectorFields(
        sessions::TSessionAccess<SceneSession> sessions,
        SceneInteractionGroup& interaction,
        Target target,
        simulation::ecs::ComponentSchema schema,
        project::ProjectCatalogAccess catalog
    )
        : sessions_(sessions), interaction_(interaction), target_(target), schema_(std::move(schema)), catalog_(catalog)
    {}
    InspectorFields::~InspectorFields() noexcept
    {
        if (!release())
            std::terminate();
    }
    InspectorFields::Status InspectorFields::withRead(const std::function<Status()>& action)
    {
        auto session = sessions_.read(interaction_.session());
        if (!session)
            return cxx::unexpected(SceneEditError{session.error()});
        auto read = session->get().read();
        if (!read)
            return cxx::unexpected(read.error());
        return read->withRead([&](const SceneReadView&) -> Status {
            // Generated/custom field copy and destruction are an extension containment boundary.
            try
            {
                return action();
            }
            catch (const std::bad_alloc&)
            {
                std::terminate();
            }
            catch (...)
            {
                return rejected(ESceneEditError::CODEC);
            }
        });
    }
    const void* InspectorFields::component(Target target, cxx::TypeToken type) const noexcept
    {
        if (!display_ || target != target_ || type != schema_.cpp_type)
            return nullptr;
        return schema_.operations.get(*display_, entity_);
    }
    std::string_view InspectorFields::writeRestriction() const noexcept
    {
        return status_ ? std::string_view{} : std::string_view{"The component is unavailable or has a pending error."};
    }
    InspectorFields::Status InspectorFields::refresh()
    {
        auto session = sessions_.read(interaction_.session());
        if (!session)
            return cxx::unexpected(SceneEditError{session.error()});
        const auto stamp = session->get().describe().current;
        if (active())
            return content_ == stamp ? Status{} : Status{rejected(ESceneEditError::STALE_CONTENT)};
        if (display_ && content_ == stamp)
            return {};
        auto read = session->get().read();
        if (!read)
            return cxx::unexpected(read.error());
        auto encoded = read->component(target_, schema_.id);
        if (!encoded)
            return cxx::unexpected(encoded.error());
        if (!schema_.decode_value || !schema_.operations.valid())
            return rejected(ESceneEditError::MISSING_SCHEMA);
        return read->withRead([&](const SceneReadView& source) -> Status {
            // Foreign codecs, candidate destruction and replacement all share this admission.
            try
            {
                auto candidate = std::make_unique<simulation::ecs::Registry>();
                if (!source.contains(target_))
                    return rejected(ESceneEditError::STALE_OBJECT);
                const auto target = candidate->create();
                struct References final
                {
                    const SceneReadView& source;
                    Target target;
                    simulation::ecs::Registry& registry;
                    std::vector<std::pair<world::WorldObjectId, simulation::ecs::Entity>> values;
                } references{source, target_, *candidate, {{target_.object, target}}};
                simulation::ecs::ComponentEntityResolver resolver{
                    &references,
                    [](const void* data, world::WorldObjectId id
                    ) noexcept -> cxx::expected<simulation::ecs::Entity, simulation::ecs::ComponentDecodeFailure> {
                        auto& references = *const_cast<References*>(static_cast<const References*>(data));
                        for (const auto& [object, entity] : references.values)
                            if (object == id)
                                return entity;
                        auto target = references.target;
                        target.object = id;
                        if (!references.source.contains(target))
                            return cxx::unexpected(simulation::ecs::ComponentDecodeFailure{
                                simulation::ecs::EComponentDecodeError::UNRESOLVED_REFERENCE,
                                0,
                                id
                            });
                        const auto entity = references.registry.create();
                        references.values.emplace_back(id, entity);
                        return entity;
                    }
                };
                auto decoded = schema_.decode_value(encoded->version, encoded->bytes, resolver, schema_.code_lifetime);
                if (!decoded)
                    return rejected(ESceneEditError::CODEC);
                std::move(*decoded).installInto(*candidate, target);
                display_ = std::move(candidate);
                entity_ = target;
                content_ = stamp;
                ++version_;
                return {};
            }
            catch (const std::bad_alloc&)
            {
                std::terminate();
            }
            catch (...)
            {
                return rejected(ESceneEditError::CODEC);
            }
        });
    }
    InspectorFields::Status InspectorFields::update()
    {
        if (active())
        {
            auto current = refresh();
            if (!current)
            {
                if (current.error().code != ESceneEditError::STALE_CONTENT)
                    return current;
                // Another author operation invalidated this preview. The old payload is discarded
                // through the existing gate; a transient access failure still retains it intact.
                auto discarded = cancel();
                if (!discarded)
                    return discarded;
                return status_ = current;
            }
        }
        if (structural_)
        {
            if (!structural_())
                return status_;
            auto cleared = withRead([&]() -> Status {
                structural_ = {};
                return {};
            });
            if (!cleared)
                return cleared;
        }
        if (active())
        {
            if (!begun_)
            {
                auto checked = refresh();
                if (!checked)
                    return checked;
                auto begun = interaction_.begin(label_);
                if (!begun)
                    return begun;
                begun_ = true;
            }
            if (!pending_.empty())
            {
                auto adopted = interaction_.preview(pending_);
                if (!adopted)
                    return adopted;
            }
            if (commit_)
            {
                auto committed = interaction_.commit();
                if (!committed)
                    return cxx::unexpected(committed.error());
                field_.clear();
                label_.clear();
                begun_ = false;
                commit_ = false;
                content_.reset();
            }
        }
        status_ = refresh();
        return status_;
    }
    bool InspectorFields::finish()
    {
        if (!active() && !structural_)
            return true;
        if (active())
            commit_ = true;
        status_ = update();
        return status_.has_value();
    }
    bool InspectorFields::queueEdit(std::function<bool()> edit)
    {
        if (structural_)
            return reject(ESceneEditError::BUSY);
        structural_ = std::move(edit);
        return true;
    }
    InspectorFields::Status InspectorFields::cancel()
    {
        if (begun_)
        {
            auto cancelled = interaction_.cancel();
            if (!cancelled)
                return cancelled;
        }
        auto cleared = withRead([&]() -> Status {
            pending_.clear();
            structural_ = {};
            field_.clear();
            label_.clear();
            begun_ = false;
            commit_ = false;
            content_.reset();
            return {};
        });
        if (!cleared)
        {
            if (!stale(cleared.error()))
                return cleared;
            pending_.clear();
            structural_ = {};
            field_.clear();
            begun_ = false;
            commit_ = false;
            return {};
        }
        return {};
    }
    InspectorFields::Status InspectorFields::release()
    {
        if (!display_ && pending_.empty() && !structural_)
            return {};
        if (begun_)
        {
            auto cancelled = interaction_.cancel();
            if (!cancelled)
                return cancelled;
        }
        const auto clear = [&]() -> Status {
            pending_.clear();
            structural_ = {};
            display_.reset();
            field_.clear();
            begun_ = false;
            commit_ = false;
            return {};
        };
        auto cleared = withRead(clear);
        if (!cleared && stale(cleared.error()))
            return clear();
        return cleared;
    }
    InspectorFields::Status InspectorFields::connectionFailure(object::EConnectError error)
    {
        if (error == object::EConnectError::ALLOCATION_FAILURE)
            std::terminate();
        return rejected(ESceneEditError::BUDGET);
    }
    InspectorFields::Status InspectorFields::constructionFailure()
    {
        return rejected(ESceneEditError::INVALID_COMPONENT);
    }
}
