#pragma once

#include <concepts>
#include <lux/engine/render/extensions/FrameDataCall.hpp>
#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/render/extensions/CommandBinding.hpp>
#include <lux/engine/render/extensions/resource/ResourceCreateContext.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <memory>

namespace lux::render
{
    using StorageOwner = std::unique_ptr<void, void (*)(void*) noexcept>;

    // Immutable implementation metadata. It has no Resource declaration or global identity.
    struct StorageMeta final
    {
        const void* ops;
        RenderResult<StorageOwner> (*create)(ResourceCreateContext&, ResourceInstanceId, const void*) noexcept;
        const void* binding_ops;
        cxx::basic_type_info config_type;
        cxx::basic_type_info ops_type;
        cxx::basic_type_info implementation_type;
    };

    struct ResourceRegistration final
    {
        const ResourceDefinition* descriptor;
        const StorageMeta* storage;
        std::span<const CommandBinding> commands;
        FrameDataCall frame_call;
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
    [[nodiscard]] const StorageMeta* makeStorageMeta(StorageOpsArgument<Ops> ops, const void* binding_ops) noexcept
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
        const void* ops_pointer = nullptr;
        if constexpr (!std::is_void_v<Ops>)
        {
            ops_pointer = &ops;
        }
        static const StorageMeta metadata{
            ops_pointer,
            [](ResourceCreateContext& dependencies, ResourceInstanceId id, const void* config
            ) noexcept -> RenderResult<StorageOwner>
            {
                auto candidate = [&]() noexcept
                {
                    if constexpr (has_config)
                    {
                        static_assert(std::same_as<
                                      decltype(Create),
                                      RenderResult<std::unique_ptr<
                                          Implementation>> (*)(ResourceCreateContext&, ResourceInstanceId, const Config&) noexcept>);
                        return Create(dependencies, id, *static_cast<const Config*>(config));
                    }
                    else
                    {
                        static_assert(std::same_as<
                                      decltype(Create),
                                      RenderResult<std::unique_ptr<
                                          Implementation>> (*)(ResourceCreateContext&, ResourceInstanceId) noexcept>);
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
            },
            binding_ops,
            config_type,
            []() noexcept
            {
                if constexpr (std::is_void_v<Ops>)
                {
                    return cxx::basic_type_info{};
                }
                else
                {
                    return cxx::make_basic_type_info<Ops>();
                }
            }(),
            cxx::make_basic_type_info<Implementation>()
        };
        const bool is_different_operations = metadata.ops != ops_pointer || metadata.binding_ops != binding_ops;
        return is_different_operations ? nullptr : &metadata;
    }

    template <class Declaration, class Implementation, auto Create>
    [[nodiscard]] ResourceRegistration makeResourceRegistration(
        StorageOpsArgument<typename Declaration::Ops> ops,
        const void* binding_ops = nullptr,
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
