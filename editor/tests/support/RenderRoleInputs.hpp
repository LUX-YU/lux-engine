#pragma once
#include <atomic>
#include <lux/engine/render/extensions/Registration.hpp>
#include <lux/engine/render/extensions/storage/StorageCreateContext.hpp>
#include <lux/engine/render/frame/RenderFrame.hpp>
#include <lux/engine/systems/render/RenderAssets.hpp>

namespace fixture
{
    struct RoleStorageDescriptor
    {
        using Config = void;
        using Ops = void;
        static inline const lux::render::StorageDescriptor
            descriptor{lux::render::StorageNameView{"test.editor.role_storage"}, 0, 0, {}, "Role isolation fixture"};
    };

    struct RoleStorage
    {
        using Descriptor = RoleStorageDescriptor;
        inline static std::atomic<unsigned> live = 0;

        lux::render::StorageId id;

        explicit RoleStorage(lux::render::StorageId id) noexcept : id(id)
        {
            ++live;
        }

        ~RoleStorage() noexcept
        {
            --live;
        }

        static lux::render::RenderResult<std::unique_ptr<RoleStorage>>
        create(lux::render::StorageCreateContext&, lux::render::StorageId id) noexcept
        {
            return std::make_unique<RoleStorage>(id);
        }

        static lux::render::StorageFactory factory() noexcept
        {
            return lux::render::
                makeStorageFactory<Descriptor, RoleStorage, lux::render::StorageCreateContext, &RoleStorage::create>(
                    nullptr
                );
        }
    };

    template <unsigned Role> struct RoleCommand
    {
        static constexpr std::string_view name = Role == 0   ? "test.role.first"
                                                 : Role == 1 ? "test.role.second"
                                                             : "test.role.view";
        static constexpr bool requires_frame_boundary = false;
        using Data = unsigned;

        struct Request
        {
            lux::render::CommandDestination destination;
            unsigned value = 0;
        };

        struct Response
        {
            lux::render::RenderError error;
            lux::render::StorageId storage;
        };
    };

    struct RoleCapability
    {
        using PrerequisiteCapabilities = std::tuple<>;
        using Commands = std::tuple<RoleCommand<0>, RoleCommand<1>, RoleCommand<2>>;
        using PerFrameData = std::tuple<>;
    };

    struct RoleHandler
    {
        static constexpr std::string_view name = "test.role.handler";

        static RoleHandler* create(lux::render::ResourceRegistry&) noexcept
        {
            return new RoleHandler;
        }

        template <unsigned Role>
        void inspect(
            RoleStorage& storage,
            const typename RoleCommand<Role>::Request&,
            typename RoleCommand<Role>::Response& out
        ) noexcept
        {
            out.storage = storage.id;
        }
    };

    struct RoleFeature
    {
        using Capability = RoleCapability;
        using Features = std::tuple<>;
        using FrameBindings = std::tuple<>;
        using Config = void;
        using Storages = std::tuple<RoleStorage>;
        using Bindings = std::tuple<
            lux::render::CommandMethod<RoleCommand<0>, &RoleHandler::inspect<0>, 0>,
            lux::render::CommandMethod<RoleCommand<1>, &RoleHandler::inspect<1>, 1>,
            lux::render::CommandMethod<RoleCommand<2>, &RoleHandler::inspect<2>, 2>>;
        static constexpr lux::render::FeatureName name{"test.editor.role_inputs"};

        static lux::render::CapabilityImplementation implementation() noexcept
        {
            using namespace lux::render;
            static const std::array inputs{
                CapabilityStorageDependency{EStorageScope::GLOBAL, RoleStorageDescriptor::descriptor.type, "a"},
                CapabilityStorageDependency{EStorageScope::GLOBAL, RoleStorageDescriptor::descriptor.type, "b"},
                CapabilityStorageDependency{EStorageScope::VIEW, RoleStorageDescriptor::descriptor.type, "view"}
            };

            static const auto value = []
            {
                using namespace lux::render;
                CapabilityImplementation result;
                result.implementation = name.name();
                return result;
            }();
            auto definition = value;
            definition.storage_dependencies = inputs;
            return definition;
        }
    };

} // namespace fixture
