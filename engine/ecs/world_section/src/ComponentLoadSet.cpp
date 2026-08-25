#include <lux/engine/ecs/ComponentLoadSet.hpp>

#include <algorithm>
#include <iterator>
#include <new>
#include <utility>
#include <vector>

namespace lux::ecs
{
    struct ComponentLoadSet::Impl final
    {
        ComponentSchemaSet schemas;
        std::vector<ComponentLoadBinding> bindings;
        std::vector<std::shared_ptr<const void>> code_lifetimes;
    };

    namespace
    {
        [[nodiscard]] WorldSectionFailure failure(
            EWorldSectionError code,
            ComponentSchemaId schema = {}
        )
        {
            WorldSectionFailure result;
            result.code = code;
            result.schema = std::move(schema);
            return result;
        }

        [[nodiscard]] bool lessBinding(
            const ComponentLoadBinding& left,
            const ComponentLoadBinding& right
        ) noexcept
        {
            if (left.schema().id.hash != right.schema().id.hash)
                return left.schema().id.hash < right.schema().id.hash;
            return left.schema().id.name < right.schema().id.name;
        }
    } // namespace

    ComponentLoadSet::ComponentLoadSet(
        std::shared_ptr<const Impl> impl
    ) noexcept
        : impl_(std::move(impl))
    {
    }

    lux::cxx::expected<ComponentLoadSet, WorldSectionFailure>
    ComponentLoadSet::build(
        const ComponentSchemaSet& schemas,
        std::span<const ComponentLoadContribution> contributions
    ) noexcept
    {
        try
        {
            auto impl = std::make_shared<Impl>();
            impl->schemas = schemas;
            std::size_t binding_count{};
            for (const auto& contribution : contributions)
            {
                binding_count += contribution.bindings.size();
            }
            struct PendingBinding final
            {
                ComponentLoadBinding binding;
                std::shared_ptr<const void> code_lifetime;
            };
            std::vector<PendingBinding> pending;
            pending.reserve(binding_count);
            for (const auto& contribution : contributions)
            {
                for (const auto& source : contribution.bindings)
                {
                    const ComponentSchema* schema = schemas.find(
                        source.schema().id
                    );
                    if (schema == nullptr ||
                        schema->version != source.schema().version ||
                        schema->cpp_type.hash() !=
                            source.schema().cpp_type.hash() ||
                        schema->cpp_type.name() !=
                            source.schema().cpp_type.name())
                    {
                        return lux::cxx::unexpected(failure(
                            EWorldSectionError::BINDING_MISMATCH,
                            source.schema().id
                        ));
                    }
                    ComponentLoadBinding binding = source;
                    binding.schema_ = schema;
                    pending.push_back(PendingBinding{
                        binding,
                        contribution.code_lifetime
                    });
                }
            }
            std::sort(
                pending.begin(),
                pending.end(),
                [](const PendingBinding& left, const PendingBinding& right)
                {
                    return lessBinding(left.binding, right.binding);
                }
            );
            impl->bindings.reserve(binding_count);
            impl->code_lifetimes.reserve(binding_count);
            for (auto& entry : pending)
            {
                impl->bindings.push_back(entry.binding);
                impl->code_lifetimes.push_back(
                    std::move(entry.code_lifetime)
                );
            }
            for (std::size_t index = 1U;
                 index < impl->bindings.size();
                 ++index)
            {
                if (impl->bindings[index - 1U].schema().id.hash ==
                    impl->bindings[index].schema().id.hash)
                {
                    return lux::cxx::unexpected(failure(
                        EWorldSectionError::DUPLICATE_BINDING,
                        impl->bindings[index].schema().id
                    ));
                }
            }
            return ComponentLoadSet(std::move(impl));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::ALLOCATION_FAILURE)
            );
        }
    }

    const ComponentLoadBinding* ComponentLoadSet::find(
        std::uint64_t schema_hash,
        std::string_view schema_name
    ) const noexcept
    {
        if (!impl_)
            return nullptr;
        const auto iterator = std::lower_bound(
            impl_->bindings.begin(),
            impl_->bindings.end(),
            std::pair{schema_hash, schema_name},
            [](const ComponentLoadBinding& binding, const auto& value)
            {
                if (binding.schema().id.hash != value.first)
                    return binding.schema().id.hash < value.first;
                return binding.schema().id.name < value.second;
            }
        );
        return iterator != impl_->bindings.end() &&
                iterator->schema().id.hash == schema_hash &&
                iterator->schema().id.name == schema_name
            ? &*iterator
            : nullptr;
    }

    const ComponentLoadBinding* ComponentLoadSet::find(
        const ComponentSchemaId& schema
    ) const noexcept
    {
        return find(schema.hash, schema.name);
    }

    std::span<const ComponentLoadBinding> ComponentLoadSet::all() const noexcept
    {
        return impl_
            ? std::span<const ComponentLoadBinding>(impl_->bindings)
            : std::span<const ComponentLoadBinding>{};
    }

    bool ComponentLoadSet::empty() const noexcept
    {
        return all().empty();
    }

    const std::shared_ptr<const void>& ComponentLoadSet::codeLifetime(
        const ComponentLoadBinding& binding
    ) const noexcept
    {
        static const std::shared_ptr<const void> empty;
        if (!impl_ || impl_->bindings.empty())
            return empty;
        const auto found = std::find_if(
            impl_->bindings.begin(),
            impl_->bindings.end(),
            [&](const ComponentLoadBinding& candidate) noexcept
            {
                return std::addressof(candidate) == std::addressof(binding);
            }
        );
        detail::require(found != impl_->bindings.end());
        return impl_->code_lifetimes[static_cast<std::size_t>(
            std::distance(impl_->bindings.begin(), found)
        )];
    }
} // namespace lux::ecs
