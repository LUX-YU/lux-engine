#pragma once

#include <lux/engine/render/data/Wire.hpp>
#include <lux/engine/render/data/ShaderInterface.hpp>

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

    struct StorageRequirement final
    {
        StorageTypeId type;
        cxx::SchemaVersion abi;
        EResourceScope scope = EResourceScope::GLOBAL;
        std::string key; // empty uses the Graph semantic key; otherwise names the scoped Storage instance
        bool operator==(const StorageRequirement&) const noexcept = default;
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
