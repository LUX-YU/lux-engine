#pragma once

#include <lux/engine/render/data/ConfigValue.hpp>
#include <lux/cxx/core/Schema.hpp>
#include <lux/engine/render/core/Ids.hpp>

namespace lux::render
{
    struct StorageNameTag;
    using StorageName = cxx::SchemaId<StorageNameTag>;
    using StorageNameView = cxx::SchemaIdView<StorageNameTag>;

    // Qualified resource identity. The local key is allocated only by its actual professional Storage.
    template <class Tag> struct StorageResourceId final
    {
        StorageId storage;
        cxx::SlotKey<Tag> local;
        [[nodiscard]] bool isNull() const noexcept { return local.isNull(); }
        bool operator==(const StorageResourceId&) const noexcept = default;
    };

    enum class EStorageCapability : std::uint32_t
    {
        NONE = 0,
        SAMPLED_IMAGE_ARRAY = 1,
        BLOCK_COMPRESSION_BC = 2
    };

    struct StorageRequirement final
    {
        StorageName type;
        std::string key; // empty uses the Graph semantic key; otherwise names the Storage instance in its actual owner
        ConfigValue config;
        bool operator==(const StorageRequirement&) const noexcept = default;
    };

    // Static metadata. A descriptor is not an instance, a resource budget or a GPU owner.
    // Config/Ops qualification belongs to the typed Factory, not a manual version or encoded config schema.
    struct StorageDescriptor final
    {
        StorageNameView type;
        std::uint32_t required_capabilities = 0;
        std::uint32_t optional_capabilities = 0;
        ConfigValue::Type binding_type;
    };

    inline constexpr auto kStorageContract = error::errorId("lux.render.resource.contract");
    inline constexpr auto kStorageMismatch = error::errorId("lux.render.resource.mismatch");
    inline constexpr auto kStorageFactory = error::errorId("lux.render.resource.factory");
    inline constexpr auto kStorageCapability = error::errorId("lux.render.resource.capability");
    inline constexpr auto kStorageIdentity = error::errorId("lux.render.resource.identity");
    inline constexpr auto kStorageCapacity = error::errorId("lux.render.resource.capacity");

    [[nodiscard]] RenderResult<void> validateStorage(const StorageDescriptor& descriptor) noexcept;
    [[nodiscard]] RenderResult<void>
    compatibleStorage(const StorageDescriptor& expected, const StorageDescriptor& actual) noexcept;

    [[nodiscard]] std::span<const error::ErrorDescriptor> renderResourceErrorDescriptors() noexcept;
}
