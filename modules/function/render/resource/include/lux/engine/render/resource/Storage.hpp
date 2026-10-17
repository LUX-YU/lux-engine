#pragma once

#include <lux/engine/render/data/Schema.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/data/ShaderInterface.hpp>

namespace lux::render
{
    struct StorageNameTag;
    using StorageName = cxx::SchemaId<StorageNameTag>;
    using StorageNameView = cxx::SchemaIdView<StorageNameTag>;

    enum class EResourceScope : std::uint8_t { GLOBAL, SCENE, VIEW, ATTACHMENT };

    enum class EStorageCapability : std::uint32_t
    {
        NONE = 0,
        SAMPLED_IMAGE_ARRAY = 1,
        BLOCK_COMPRESSION_BC = 2
    };

    struct StorageRequirement final
    {
        StorageName type;
        EResourceScope scope = EResourceScope::GLOBAL;
        std::string key; // empty uses the Graph semantic key; otherwise names the scoped Storage instance
        bool operator==(const StorageRequirement&) const noexcept = default;
    };

    // Static metadata. A descriptor is not an instance, a resource budget or a GPU owner.
    // Config/Ops qualification belongs to the typed Factory, not a manual version or encoded config schema.
    struct StorageDescriptor final
    {
        StorageNameView type;
        std::uint32_t required_capabilities = 0;
        std::uint32_t optional_capabilities = 0;
        std::span<const ShaderBinding> bindings;
        const DataSchemaDescriptor* binding_schema = nullptr;
    };

    inline constexpr auto kStorageContract = error::errorId("lux.render.resource.contract");
    inline constexpr auto kStorageMismatch = error::errorId("lux.render.resource.mismatch");
    inline constexpr auto kStorageFactory = error::errorId("lux.render.resource.factory");
    inline constexpr auto kStorageCapability = error::errorId("lux.render.resource.capability");
    inline constexpr auto kStorageDomain = error::errorId("lux.render.resource.domain");
    inline constexpr auto kStorageCapacity = error::errorId("lux.render.resource.capacity");

    [[nodiscard]] RenderResult<void> validateStorage(const StorageDescriptor& descriptor) noexcept;
    [[nodiscard]] RenderResult<void>
    compatibleStorage(const StorageDescriptor& expected, const StorageDescriptor& actual) noexcept;

    [[nodiscard]] std::span<const error::ErrorDescriptor> renderResourceErrorDescriptors() noexcept;
}
