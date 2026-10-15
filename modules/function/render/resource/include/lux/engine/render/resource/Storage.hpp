#pragma once

#include <lux/engine/render/data/Wire.hpp>

namespace lux::render
{
    struct StorageTypeTag;
    using StorageTypeId = cxx::SchemaId<StorageTypeTag>;
    using StorageTypeIdView = cxx::SchemaIdView<StorageTypeTag>;

    enum class EResourceScope : std::uint8_t { GLOBAL, SCENE, VIEW, ATTACHMENT };

    [[nodiscard]] constexpr std::uint8_t scopeMask(EResourceScope scope) noexcept
    {
        return std::uint8_t{1} << static_cast<unsigned>(scope);
    }

    enum class EStorageCapability : std::uint32_t
    {
        NONE = 0,
        SAMPLED_IMAGE_ARRAY = 1,
        BLOCK_COMPRESSION_BC = 2
    };

    enum class EDescriptorKind : std::uint8_t { SAMPLED_IMAGE_ARRAY, STORAGE_BUFFER };

    // Fragment-stage descriptor contract of the current cooked graphics path. A zero count denotes
    // a runtime array; its physical capacity is chosen by the registered Storage configuration.
    struct ShaderBinding final
    {
        std::uint32_t set = 0, binding = 0;
        EDescriptorKind kind = EDescriptorKind::SAMPLED_IMAGE_ARRAY;
        std::uint32_t count = 0;
        std::uint32_t layout_bytes = 0; // current StorageBuffer primitive: one float4 at offset zero (16 bytes)
        bool operator==(const ShaderBinding&) const noexcept = default;
    };

    struct StorageRequirement final
    {
        StorageTypeId type;
        cxx::SchemaVersion abi;
        EResourceScope scope = EResourceScope::GLOBAL;
        std::string key; // empty uses the Graph semantic key; otherwise names the scoped Storage instance
        bool operator==(const StorageRequirement&) const noexcept = default;
    };

    // Effect-owned concrete arguments, resolved only at Attach; never part of immutable Native code.
    struct StorageArgument final
    {
        std::string resource_key;
        OwnedWireBytes bytes;
    };

    // Static metadata. A descriptor is not an instance, a resource budget or a GPU owner.
    // Configuration uses the existing DataSchema wire contract, never a C++ object representation.
    struct StorageDescriptor final
    {
        StorageTypeIdView type;
        cxx::SchemaVersion abi;
        std::uint8_t scopes = scopeMask(EResourceScope::GLOBAL);
        const DataSchemaDescriptor* config = nullptr;
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
