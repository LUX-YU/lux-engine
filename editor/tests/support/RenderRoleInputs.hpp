#pragma once
#include <atomic>
#include <lux/engine/context/render/RenderAssets.hpp>
#include <lux/engine/render/extensions/FeatureRegistration.hpp>
#include <lux/engine/render/extensions/storage/StorageCreateContext.hpp>
#include <lux/engine/scene/render/RenderBindingPlan.hpp>

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

        RoleStorage() noexcept
        {
            ++live;
        }

        ~RoleStorage() noexcept
        {
            --live;
        }

        static lux::render::RenderResult<std::unique_ptr<RoleStorage>> create(
            lux::render::StorageCreateContext&,
            lux::render::StorageId
        ) noexcept
        {
            return std::make_unique<RoleStorage>();
        }

        static lux::render::StorageFactory factory() noexcept
        {
            return lux::render::
                makeStorageFactory<Descriptor, RoleStorage, lux::render::StorageCreateContext, &RoleStorage::create>(
                    nullptr
                );
        }
    };

    struct RoleFeature
    {
        using Config = void;
        using Storages = std::tuple<RoleStorage>;
        using Commands = std::tuple<>;
        static constexpr lux::render::FeatureName name{"test.editor.role_inputs"};

        static const lux::render::FeatureGraphOps& graph(bool) noexcept
        {
            using namespace lux::render;
            static const std::array inputs{
                CapabilityStorageInput{
                    "test.role",
                    1,
                    ECapabilityInputScope::GLOBAL,
                    RoleStorageDescriptor::descriptor.type,
                    "a"
                },
                CapabilityStorageInput{
                    "test.role",
                    1,
                    ECapabilityInputScope::GLOBAL,
                    RoleStorageDescriptor::descriptor.type,
                    "b"
                },
                CapabilityStorageInput{
                    "test.role",
                    1,
                    ECapabilityInputScope::VIEW,
                    RoleStorageDescriptor::descriptor.type,
                    "view"
                }
            };
            static const FeatureGraphOps value{.storage_inputs = inputs};
            return value;
        }

        static const lux::render::CapabilitySupport& capabilities() noexcept
        {
            static const auto value = []
            {
                using namespace lux::render;
                CapabilitySupport result;
                result.implementation = name.name();
                RenderCapability capability;
                capability.name = name.name();
                RenderPayloadContract input;
                input.schema = "test.role";
                input.use = ERenderPayloadUse::RESOURCE_INPUT;
                input.setCppType(lux::cxx::typeToken<unsigned>());
                capability.inputs.push_back(input);
                result.capabilities.push_back(std::move(capability));
                return result;
            }();
            return value;
        }
    };

} // namespace fixture
