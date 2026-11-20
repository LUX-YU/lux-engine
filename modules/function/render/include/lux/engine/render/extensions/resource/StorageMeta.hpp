#pragma once

#include <concepts>
#include <lux/engine/render/extensions/FrameDataCall.hpp>
#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/render/extensions/CommandBinding.hpp>
#include <lux/engine/render/extensions/resource/ResourceCreateContext.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <memory>
#include <optional>
#include <utility>

namespace lux::render
{
    using StorageOwner = std::unique_ptr<void, void (*)(void*) noexcept>;

    // Immutable implementation metadata. It has no Resource declaration or global identity.
    struct StorageMeta final
    {
        StorageMeta() noexcept = default;
        StorageMeta(const StorageMeta&) = delete;
        StorageMeta& operator=(const StorageMeta&) = delete;

        std::unique_ptr<const void, void (*)(const void*) noexcept> ops{nullptr, nullptr};
        RenderResult<StorageOwner> (*create)(ResourceCreateContext&, ResourceInstanceId, const void*) noexcept = nullptr;
        std::optional<detail::StorageSnapshotOps> snapshot_ops;
        std::optional<detail::StorageBindingOps> binding_ops;
        cxx::basic_type_info config_type;
        cxx::basic_type_info ops_type;
        cxx::basic_type_info implementation_type;

        [[nodiscard]] const detail::StorageBindingOps* bindings() const noexcept
        {
            return binding_ops ? &*binding_ops : nullptr;
        }
    };

    struct ResourceRegistration final
    {
        ResourceRegistration(
            const ResourceDefinition* definition,
            std::unique_ptr<StorageMeta> metadata,
            std::span<const CommandBinding> bindings,
            FrameDataCall inputs
        ) noexcept
            : descriptor(definition), commands(bindings), frame_call(inputs), storage_(std::move(metadata))
        {
        }

        [[nodiscard]] const StorageMeta* storage() const & noexcept { return storage_.get(); }
        [[nodiscard]] const StorageMeta* storage() const && = delete;

        const ResourceDefinition* descriptor;
        std::span<const CommandBinding> commands;
        FrameDataCall frame_call;

    private:
        friend class ResourceRegistry;
        std::unique_ptr<StorageMeta> storage_;
    };

    template <class Declaration> [[nodiscard]] constexpr cxx::basic_type_info storageOpsType() noexcept
    {
        if constexpr (std::is_void_v<typename Declaration::Ops>)
        {
            return {};
        }
        else
        {
            return cxx::make_basic_type_info<typename Declaration::Ops>();
        }
    }

    template <class Ops>
    using StorageOpsArgument =
        std::conditional_t<std::is_void_v<Ops>, std::nullptr_t, std::add_lvalue_reference_t<std::add_const_t<Ops>>>;

    template <class Config, class Ops, class Implementation, auto Create>
    [[nodiscard]] std::unique_ptr<StorageMeta>
    makeStorageMeta(StorageOpsArgument<Ops> ops, const detail::StorageBindingOps* binding_ops) noexcept
    {
        constexpr bool has_config = !std::is_void_v<Config>;
        constexpr cxx::basic_type_info config_type = []() noexcept
        {
            if constexpr (has_config)
            {
                return cxx::make_basic_type_info<Config>();
            }
            else
            {
                return cxx::basic_type_info{};
            }
        }();
        auto metadata = std::make_unique<StorageMeta>();
        if constexpr (!std::is_void_v<Ops>)
        {
            static_assert(std::is_nothrow_copy_constructible_v<Ops> && std::is_nothrow_destructible_v<Ops>);
            metadata->ops = {
                new const Ops(ops),
                [](const void* owner) noexcept { delete static_cast<const Ops*>(owner); }
            };
            metadata->ops_type = cxx::make_basic_type_info<Ops>();
        }
        if (binding_ops)
        {
            metadata->binding_ops = *binding_ops;
            if (binding_ops->cpu_snapshot)
            {
                metadata->snapshot_ops = *binding_ops->cpu_snapshot;
                metadata->binding_ops->cpu_snapshot = &*metadata->snapshot_ops;
            }
        }
        metadata->config_type = config_type;
        metadata->implementation_type = cxx::make_basic_type_info<Implementation>();
        metadata->create = [](ResourceCreateContext& dependencies, ResourceInstanceId id, const void* config)
            noexcept -> RenderResult<StorageOwner>
        {
            auto candidate = [&]() noexcept
            {
                if constexpr (has_config)
                {
                    using Factory = RenderResult<std::unique_ptr<Implementation>> (*)(
                        ResourceCreateContext&, ResourceInstanceId, const Config&
                    ) noexcept;
                    static_assert(std::same_as<decltype(Create), Factory>);
                    return Create(dependencies, id, *static_cast<const Config*>(config));
                }
                else
                {
                    using Factory = RenderResult<std::unique_ptr<Implementation>> (*)(
                        ResourceCreateContext&, ResourceInstanceId
                    ) noexcept;
                    static_assert(std::same_as<decltype(Create), Factory>);
                    return Create(dependencies, id);
                }
            }();
            if (!candidate)
            {
                return cxx::unexpected(candidate.error());
            }
            return StorageOwner{
                candidate->release(),
                [](void* value) noexcept { delete static_cast<Implementation*>(value); }
            };
        };
        return metadata;
    }

    template <class Declaration, class Implementation, auto Create>
    [[nodiscard]] ResourceRegistration makeResourceRegistration(
        StorageOpsArgument<typename Declaration::Ops> ops,
        const detail::StorageBindingOps* binding_ops = nullptr,
        std::span<const CommandBinding> commands = {},
        FrameDataCall frame_call = {}
    ) noexcept
    {
        return {
            &Declaration::descriptor,
            makeStorageMeta<
                typename Declaration::Config,
                typename Declaration::Ops,
                Implementation,
                Create>(ops, binding_ops),
            commands,
            frame_call
        };
    }

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void>
    validateResourceRegistration(const ResourceRegistration&) noexcept;
} // namespace lux::render
