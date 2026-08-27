#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>

#include <algorithm>
#include <utility>

namespace lux::simulation::ecs
{
    struct ComponentSchemaSet::Impl final
    {
        std::vector<ComponentSchema> schemas;
    };

    namespace
    {
        [[nodiscard]] lux::cxx::expected<void, SchemaFailure>
        validate(std::span<const ComponentSchema> schemas) noexcept
        {
            for (std::size_t index{}; index < schemas.size(); ++index)
            {
                const ComponentSchema& schema = schemas[index];
                if (!schema.id.valid())
                    return lux::cxx::unexpected(
                        SchemaFailure{ESchemaError::INVALID_SCHEMA_ID, schema.id, schema.cpp_type}
                    );
                if (!schema.cpp_type.isValid())
                    return lux::cxx::unexpected(
                        SchemaFailure{ESchemaError::INVALID_CPP_TYPE, schema.id, schema.cpp_type}
                    );
                if (schema.version == 0)
                    return lux::cxx::unexpected(
                        SchemaFailure{ESchemaError::INVALID_VERSION, schema.id, schema.cpp_type}
                    );

                const ComponentOperations& operations = schema.operations;
                if (!operations.valid())
                {
                    return lux::cxx::unexpected(
                        SchemaFailure{ESchemaError::INVALID_OPERATIONS, schema.id, schema.cpp_type}
                    );
                }
                for (std::size_t other = index + 1; other < schemas.size(); ++other)
                {
                    const ComponentSchema& right = schemas[other];
                    if (schema.id.hash == right.id.hash)
                    {
                        const auto code = schema.id.name == right.id.name ? ESchemaError::DUPLICATE_SCHEMA_ID
                                                                          : ESchemaError::SCHEMA_ID_COLLISION;
                        return lux::cxx::unexpected(SchemaFailure{code, schema.id, schema.cpp_type});
                    }
                    if (schema.cpp_type.hash() == right.cpp_type.hash())
                    {
                        const auto code = schema.cpp_type.name() == right.cpp_type.name()
                                              ? ESchemaError::DUPLICATE_CPP_TYPE
                                              : ESchemaError::CPP_TYPE_COLLISION;
                        return lux::cxx::unexpected(SchemaFailure{code, schema.id, schema.cpp_type});
                    }
                }
            }
            return {};
        }
    } // namespace

    ComponentSchemaSet::ComponentSchemaSet(std::shared_ptr<const Impl> impl) noexcept : impl_(std::move(impl))
    {
    }

    lux::cxx::expected<ComponentSchemaSet, SchemaFailure>
    ComponentSchemaSet::build(std::vector<ComponentSchema> schemas) noexcept
    {
        if (auto result = validate(schemas); !result)
            return lux::cxx::unexpected(result.error());

        try
        {
            auto impl = std::make_shared<Impl>();
            impl->schemas = std::move(schemas);
            std::sort(
                impl->schemas.begin(),
                impl->schemas.end(),
                [](const ComponentSchema& left, const ComponentSchema& right) {
                    if (left.id.hash != right.id.hash)
                        return left.id.hash < right.id.hash;
                    return left.id.name < right.id.name;
                }
            );
            return ComponentSchemaSet(std::move(impl));
        }
        catch (...)
        {
            return lux::cxx::unexpected(SchemaFailure{ESchemaError::ALLOCATION_FAILURE});
        }
    }

    lux::cxx::expected<ComponentSchemaSet, SchemaFailure> ComponentSchemaSet::build(
        std::span<const ComponentSchema> schemas,
        std::shared_ptr<const void> code_lifetime
    ) noexcept
    {
        try
        {
            std::vector<ComponentSchema> pinned(schemas.begin(), schemas.end());
            for (ComponentSchema& schema : pinned)
                schema.code_lifetime = code_lifetime;
            return build(std::move(pinned));
        }
        catch (...)
        {
            return lux::cxx::unexpected(SchemaFailure{ESchemaError::ALLOCATION_FAILURE});
        }
    }

    lux::cxx::expected<ComponentSchemaSet, SchemaFailure>
    ComponentSchemaSet::extended(std::span<const ComponentSchema> schemas) const noexcept
    {
        try
        {
            std::vector<ComponentSchema> combined;
            combined.reserve(all().size() + schemas.size());
            combined.insert(combined.end(), all().begin(), all().end());
            combined.insert(combined.end(), schemas.begin(), schemas.end());
            return build(std::move(combined));
        }
        catch (...)
        {
            return lux::cxx::unexpected(SchemaFailure{ESchemaError::ALLOCATION_FAILURE});
        }
    }

    lux::cxx::expected<ComponentSchemaSet, SchemaFailure> ComponentSchemaSet::extended(
        std::span<const ComponentSchema> schemas,
        std::shared_ptr<const void> code_lifetime
    ) const noexcept
    {
        try
        {
            std::vector<ComponentSchema> pinned(schemas.begin(), schemas.end());
            for (ComponentSchema& schema : pinned)
                schema.code_lifetime = code_lifetime;
            return extended(pinned);
        }
        catch (...)
        {
            return lux::cxx::unexpected(SchemaFailure{ESchemaError::ALLOCATION_FAILURE});
        }
    }

    const ComponentSchema* ComponentSchemaSet::find(const ComponentSchemaId& id) const noexcept
    {
        if (!impl_)
            return nullptr;
        const auto iterator = std::lower_bound(
            impl_->schemas.begin(),
            impl_->schemas.end(),
            id,
            [](const ComponentSchema& schema, const ComponentSchemaId& value) {
                if (schema.id.hash != value.hash)
                    return schema.id.hash < value.hash;
                return schema.id.name < value.name;
            }
        );
        return iterator != impl_->schemas.end() && iterator->id == id ? &*iterator : nullptr;
    }

    const ComponentSchema* ComponentSchemaSet::find(lux::cxx::TypeToken type) const noexcept
    {
        if (!impl_)
            return nullptr;
        const auto iterator =
            std::find_if(impl_->schemas.begin(), impl_->schemas.end(), [type](const ComponentSchema& schema) {
                return schema.cpp_type.hash() == type.hash() && schema.cpp_type.name() == type.name();
            }
            );
        return iterator == impl_->schemas.end() ? nullptr : &*iterator;
    }

    std::span<const ComponentSchema> ComponentSchemaSet::all() const noexcept
    {
        return impl_ ? std::span<const ComponentSchema>(impl_->schemas) : std::span<const ComponentSchema>{};
    }

    bool ComponentSchemaSet::empty() const noexcept
    {
        return all().empty();
    }
} // namespace lux::simulation::ecs
