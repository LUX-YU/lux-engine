#include <lux/engine/simulation/ecs/ComponentSnapshotSet.hpp>

#include <lux/engine/simulation/ecs/schema/ComponentOperationsAccess.hpp>
#include <lux/engine/simulation/ecs/snapshot/detail/ComponentSnapshotSetAccess.hpp>

#include <algorithm>
#include <new>
#include <utility>
#include <vector>

namespace lux::simulation::ecs
{
    struct ComponentSnapshotSet::Impl final
    {
        ComponentSchemaSet schemas;
        std::vector<ComponentSnapshotBinding> bindings;
        std::vector<std::shared_ptr<const void>> code_lifetimes;
    };

    namespace
    {
        [[nodiscard]] std::uint64_t storageKey(
            const ComponentSnapshotBinding& binding
        ) noexcept
        {
            return detail::ComponentOperationsAccess::storageKey(
                binding.schema().operations
            );
        }

        [[nodiscard]] SnapshotError failure(
            ESnapshotError code,
            std::uint64_t storage = 0U,
            ComponentSchemaId schema = {}
        )
        {
            return SnapshotError{code, storage, std::move(schema)};
        }
    } // namespace

    ComponentSnapshotSet::ComponentSnapshotSet(
        std::shared_ptr<const Impl> impl
    ) noexcept
        : impl_(std::move(impl))
    {
    }

    lux::cxx::expected<ComponentSnapshotSet, SnapshotError>
    ComponentSnapshotSet::build(
        const ComponentSchemaSet& schemas,
        std::span<const ComponentSnapshotContribution> contributions
    ) noexcept
    {
        try
        {
            auto impl = std::make_shared<Impl>();
            impl->schemas = schemas;
            std::size_t count{};
            for (const auto& contribution : contributions)
            {
                count += contribution.bindings.size();
                if (contribution.code_lifetime)
                    impl->code_lifetimes.push_back(contribution.code_lifetime);
            }
            impl->bindings.reserve(count);
            for (const auto& contribution : contributions)
            {
                for (const auto& source : contribution.bindings)
                {
                    const ComponentSchema* schema = schemas.find(
                        source.schema().id
                    );
                    if (schema == nullptr ||
                        schema->snapshot != EComponentSnapshotPolicy::COPY ||
                        schema->version != source.schema().version ||
                        schema->cpp_type.hash() !=
                            source.schema().cpp_type.hash() ||
                        schema->cpp_type.name() !=
                            source.schema().cpp_type.name())
                    {
                        return lux::cxx::unexpected(failure(
                            ESnapshotError::BINDING_MISMATCH,
                            storageKey(source),
                            source.schema().id
                        ));
                    }
                    ComponentSnapshotBinding binding = source;
                    binding.schema_ = schema;
                    impl->bindings.push_back(binding);
                }
            }
            std::sort(
                impl->bindings.begin(),
                impl->bindings.end(),
                [](const auto& left, const auto& right)
                {
                    return storageKey(left) < storageKey(right);
                }
            );
            for (std::size_t index = 1U;
                 index < impl->bindings.size();
                 ++index)
            {
                if (storageKey(impl->bindings[index - 1U]) ==
                    storageKey(impl->bindings[index]))
                {
                    return lux::cxx::unexpected(failure(
                        ESnapshotError::DUPLICATE_BINDING,
                        storageKey(impl->bindings[index]),
                        impl->bindings[index].schema().id
                    ));
                }
            }
            return ComponentSnapshotSet(std::move(impl));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                failure(ESnapshotError::ALLOCATION_FAILURE)
            );
        }
    }

    std::span<const ComponentSnapshotBinding>
    ComponentSnapshotSet::all() const noexcept
    {
        return impl_
            ? std::span<const ComponentSnapshotBinding>(impl_->bindings)
            : std::span<const ComponentSnapshotBinding>{};
    }

    bool ComponentSnapshotSet::empty() const noexcept
    {
        return all().empty();
    }

    const ComponentSchemaSet& detail::ComponentSnapshotSetAccess::schemas(
        const ComponentSnapshotSet& set
    ) noexcept
    {
        detail::require(set.impl_ != nullptr);
        return set.impl_->schemas;
    }

    const ComponentSnapshotBinding*
    detail::ComponentSnapshotSetAccess::findStorage(
        const ComponentSnapshotSet& set,
        std::uint64_t storage
    ) noexcept
    {
        if (!set.impl_)
            return nullptr;
        const auto iterator = std::lower_bound(
            set.impl_->bindings.begin(),
            set.impl_->bindings.end(),
            storage,
            [](const auto& binding, std::uint64_t value)
            {
                return storageKey(binding) < value;
            }
        );
        return iterator != set.impl_->bindings.end() &&
                storageKey(*iterator) == storage
            ? &*iterator
            : nullptr;
    }
} // namespace lux::simulation::ecs
