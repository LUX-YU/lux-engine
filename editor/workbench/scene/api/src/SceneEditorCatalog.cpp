#include <lux/engine/editor/scene/SceneEditorCatalog.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <set>
#include <unordered_map>

namespace lux::editor::scene
{
    namespace
    {
        constexpr services::ServiceContract contracts[]{
            services::ServiceContract::forType<SceneEditorCatalog, SceneEditorCatalog>(
                services::ServiceNameView{"lux.editor.scene.editors"}
            )
        };
        [[nodiscard]] auto reject(services::EServiceError code, std::string domain, std::string detail = {}) noexcept
        {
            return cxx::unexpected(services::ServiceFailure{code, std::move(detail), std::move(domain)});
        }
    } // namespace
    SceneEditorCatalog::SceneEditorCatalog(std::shared_ptr<const Definition> definition) noexcept
        : definition_(std::move(definition))
    {
    }
    const SceneEditorCatalog::Definition& SceneEditorCatalog::definition() const noexcept
    {
        return *definition_;
    }
    services::ServiceResult<std::unique_ptr<SceneEditorCatalog>> SceneEditorCatalog::
        create(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
    {
        auto definition = resolver.definition<Definition>();
        if (!definition)
        {
            return cxx::unexpected(std::move(definition.error()));
        }
        return std::make_unique<SceneEditorCatalog>(std::move(*definition));
    }
    std::shared_ptr<const services::ServiceEntry> declareSceneEditors(
        object::CodeLease code,
        services::ServiceNameView implementation,
        SceneEditorCatalog::Definition definition
    )
    {
        for (auto& item : definition.configurations)
        {
            item.code = code;
        }
        for (auto& item : definition.components)
        {
            item.code = object::pinCodeOwner(code, std::move(item.code));
        }
        auto descriptor = services::ServiceDescriptor::forType<SceneEditorCatalog, &SceneEditorCatalog::create>(
            implementation,
            contracts
        );
        descriptor.definition_type = cxx::typeToken<SceneEditorCatalog::Definition>();
        return services::ServiceEntry::create(
            std::move(code),
            descriptor,
            std::make_shared<const SceneEditorCatalog::Definition>(std::move(definition))
        );
    }
    services::ServiceResult<std::vector<std::shared_ptr<const SceneEditorCatalog::Definition>>> sceneEditorDefinitions(
        std::span<const std::shared_ptr<const services::ServiceEntry>> entries
    ) noexcept
    {
        std::vector<std::shared_ptr<const SceneEditorCatalog::Definition>> result;
        for (const auto& entry : entries)
        {
            if (!entry)
            {
                return reject(services::EServiceError::INVALID_DESCRIPTOR, "scene.editors");
            }
            for (const auto& contract : entry->descriptor().contracts)
            {
                if (contract.id.name() != contracts[0].id.name())
                {
                    continue;
                }
                auto definition = entry->definition<SceneEditorCatalog::Definition>();
                if (!definition)
                {
                    return cxx::unexpected(std::move(definition.error()));
                }
                result.push_back(std::move(*definition));
            }
        }
        return result;
    }
    services::ServiceResult<void> validateSceneEditors(
        meta::ReflectionRegistry& reflection,
        std::span<const std::shared_ptr<const services::ServiceEntry>> entries
    ) noexcept
    {
        auto definitions = sceneEditorDefinitions(entries);
        if (!definitions)
        {
            return cxx::unexpected(std::move(definitions.error()));
        }
        std::set<std::pair<std::string_view, std::uint32_t>> configurations;
        std::unordered_map<std::uint64_t, std::string_view> components;
        for (const auto& definition : *definitions)
        {
            const bool is_over_capacity =
                definition->configurations.size() > 256 || definition->components.size() > 256;
            if (is_over_capacity)
            {
                return reject(services::EServiceError::CAPACITY, "scene.editors");
            }
            for (const auto& item : definition->configurations)
            {
                const bool is_invalid_value = item.value.schema_name.empty() || !item.value.schema_version ||
                                              !item.value.codec.valid() || !item.value.reflection;
                const bool is_invalid_factory = !item.code.valid() || !item.create;
                const bool is_invalid = is_invalid_value || is_invalid_factory;
                if (is_invalid)
                {
                    return reject(services::EServiceError::INVALID_DESCRIPTOR, "configuration");
                }
                if (!configurations.emplace(item.value.schema_name, item.value.schema_version).second)
                {
                    return reject(services::EServiceError::DUPLICATE, "configuration");
                }
                if (configurations.size() > 256)
                {
                    return reject(services::EServiceError::CAPACITY, "configuration");
                }
                const auto* type = item.value.reflection(reflection);
                const bool is_type_mismatch = !type || type->type.ptr != type ||
                                              type->type.hash != item.value.codec.type.hash() ||
                                              type->type.name != item.value.codec.type.name();
                if (is_type_mismatch)
                {
                    return reject(
                        services::EServiceError::TYPE_MISMATCH,
                        "configuration.reflection",
                        item.value.schema_name
                    );
                }
            }
            for (const auto& item : definition->components)
            {
                const bool is_invalid = !item.type.isValid() || !item.create || item.label.empty();
                if (is_invalid)
                {
                    return reject(services::EServiceError::INVALID_DESCRIPTOR, "component");
                }
                auto [existing, inserted] = components.emplace(item.type.hash(), item.type.name());
                if (!inserted)
                {
                    const auto code = existing->second == item.type.name() ? services::EServiceError::DUPLICATE
                                                                         : services::EServiceError::HASH_COLLISION;
                    return reject(code, "component");
                }
                if (components.size() > 256)
                {
                    return reject(services::EServiceError::CAPACITY, "component");
                }
            }
        }
        return {};
    }
} // namespace lux::editor::scene
