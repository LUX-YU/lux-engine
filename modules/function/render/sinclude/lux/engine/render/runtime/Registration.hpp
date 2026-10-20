#pragma once

#include <lux/engine/render/resource/StorageFactory.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>

namespace lux::render
{

    // DLL specializations are visible before generic registration is instantiated.
    class SampledTexture2DStorageDescriptor;
    class IndexedTriangleMeshStorageDescriptor;
    class RasterMaterialTableStorageDescriptor;
    class RasterMeshInstanceStorageDescriptor;
    struct MaterialMeshRasterDescriptor;
    struct MeshDepthRasterDescriptor;

    template <>
    LUX_RENDER_PUBLIC RenderResult<void> RenderServer::prepareStorage<SampledTexture2DStorageDescriptor>(
        RegistrationBatch&,
        ConfigValue,
        bool,
        std::string_view
    ) noexcept;

    template <>
    LUX_RENDER_PUBLIC RenderResult<void> RenderServer::prepareStorage<IndexedTriangleMeshStorageDescriptor>(
        RegistrationBatch&,
        ConfigValue,
        bool,
        std::string_view
    ) noexcept;

    template <>
    LUX_RENDER_PUBLIC RenderResult<void> RenderServer::prepareStorage<RasterMaterialTableStorageDescriptor>(
        RegistrationBatch&,
        ConfigValue,
        bool,
        std::string_view
    ) noexcept;

    template <>
    LUX_RENDER_PUBLIC RenderResult<void> RenderServer::prepareStorage<RasterMeshInstanceStorageDescriptor>(
        RegistrationBatch&,
        ConfigValue,
        bool,
        std::string_view
    ) noexcept;

    template <>
    LUX_RENDER_PUBLIC RenderResult<void> RenderServer::prepareFeature<MaterialMeshRasterDescriptor>(
        RegistrationBatch&,
        std::string_view
    ) noexcept;

    template <>
    LUX_RENDER_PUBLIC RenderResult<void> RenderServer::prepareFeature<MeshDepthRasterDescriptor>(
        RegistrationBatch&,
        std::string_view
    ) noexcept;

    template <class Descriptor>
        requires(!std::is_void_v<typename Descriptor::Config>)
    RenderResult<StorageTypeId> RenderServer::registerStorage(
        const typename Descriptor::Config& config,
        std::string_view handler_key
    ) noexcept
    {
        auto value = ConfigValue::make(config);
        if (!value)
        {
            return cxx::unexpected(value.error());
        }
        RegistrationBatch batch;
        if (auto prepared = prepareStorage<Descriptor>(batch, std::move(*value), false, handler_key); !prepared)
        {
            return cxx::unexpected(prepared.error());
        }
        if (auto published = install(batch); !published)
        {
            return cxx::unexpected(published.error());
        }
        return batch.storageId();
    }

    template <class Descriptor>
        requires std::is_void_v<typename Descriptor::Config>
    RenderResult<StorageTypeId> RenderServer::registerStorage(std::string_view handler_key) noexcept
    {
        RegistrationBatch batch;
        if (auto prepared = prepareStorage<Descriptor>(batch, {}, false, handler_key); !prepared)
        {
            return cxx::unexpected(prepared.error());
        }
        if (auto published = install(batch); !published)
        {
            return cxx::unexpected(published.error());
        }
        return batch.storageId();
    }

    template <class Descriptor>
    RenderResult<FeatureId> RenderServer::registerFeature(std::string_view handler_key) noexcept
    {
        RegistrationBatch batch;
        if (auto prepared = prepareFeature<Descriptor>(batch, handler_key); !prepared)
        {
            return cxx::unexpected(prepared.error());
        }
        if (auto published = install(batch); !published)
        {
            return cxx::unexpected(published.error());
        }
        return batch.featureId();
    }

    template <class Descriptor, class Storage, class Handler>
    RenderResult<void> RenderServer::prepareStorageDefinition(
        RegistrationBatch& batch,
        StorageFactory factory,
        ConfigValue configuration,
        bool use_existing,
        std::string_view handler_key
    ) noexcept
    {
        using Config = typename Descriptor::Config;
        constexpr CppType config_type = []() noexcept
        {
            if constexpr (std::is_void_v<Config>)
            {
                return CppType{};
            }
            else
            {
                static_assert(requires(const Config& a, const Config& b) {
                    { a == b } noexcept -> std::same_as<bool>;
                });
                return cppType<Config>();
            }
        }();
        if (!factory.descriptor)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        if (auto valid = compatibleStorage(Descriptor::descriptor, *factory.descriptor); !valid)
        {
            return valid;
        }
        const bool is_wrong_definition = factory.native_type != cppType<Storage>() ||
                                         factory.config_type != config_type ||
                                         factory.ops_type != storageOpsType<Descriptor>();
        if (is_wrong_definition)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        batch.addStorage(std::move(factory), std::move(configuration), use_existing);
        if constexpr (!std::is_void_v<Handler>)
        {
            return prepareHandler<Handler>(
                batch,
                handler_key,
                [&]() noexcept { return std::make_unique<Handler>(resources()); }
            );
        }
        return {};
    }
} // namespace lux::render
