#include <lux/engine/render/runtime/RegistrationBatch.hpp>

namespace lux::render
{
    RenderServer::RegistrationBatch::RegistrationBatch() noexcept : state_(std::make_unique<State>()) {}

    RenderServer::RegistrationBatch::~RegistrationBatch() noexcept = default;

    RenderResult<bool> RenderServer::RegistrationBatch::containsHandler(CppType type, std::string_view instance)
        const noexcept
    {
        const auto found = state_->handlers_by_name.find({type.token.name(), instance});
        if (found == state_->handlers_by_name.end())
        {
            return false;
        }
        if (state_->handlers[found->second].record->type != type)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        return true;
    }

    void RenderServer::RegistrationBatch::addHandler(
        CppType type,
        std::string_view name,
        std::string_view instance,
        std::unique_ptr<void, void (*)(void*) noexcept> object,
        std::vector<CommandBinding> calls
    ) noexcept
    {
        auto record =
            std::make_unique<HandlerRecord>(type, std::string(name), std::string(instance), std::move(object));
        state_->handlers_by_name.emplace(
            State::HandlerName{type.token.name(), record->instance_name},
            state_->handlers.size()
        );
        state_->handlers.push_back({std::move(record), std::move(calls)});
    }

    void RenderServer::RegistrationBatch::addStorage(
        StorageFactory factory,
        ConfigValue configuration,
        bool use_existing
    ) noexcept
    {
        state_->storage_factories.push_back({std::move(factory), std::move(configuration), use_existing, {}});
    }

    void RenderServer::RegistrationBatch::addFeature(FeatureDefinition definition) noexcept
    {
        state_->features.push_back({definition, {}});
    }

    StorageTypeId RenderServer::RegistrationBatch::storageId() const noexcept
    {
        return state_->storage_factories.front().id;
    }

    FeatureId RenderServer::RegistrationBatch::featureId() const noexcept
    {
        return state_->features.front().id;
    }

} // namespace lux::render
