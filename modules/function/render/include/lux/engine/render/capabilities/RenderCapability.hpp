#pragma once

#include <lux/engine/render/capabilities/RenderPayloadContract.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace lux::render
{
    class RenderServer;
    class ResourceRegistry;
    class StorageSet;

    // A choice of a stable implementation entry in this Server. Never serialize it;
    // persist the implementation name/version and resolve them again when opening a session.
    class CapabilitySelection final
    {
    public:
        CapabilitySelection() noexcept = default;

        [[nodiscard]] bool isNull() const noexcept
        {
            return implementation_.isNull();
        }

        bool operator==(const CapabilitySelection&) const noexcept = default;

    private:
        friend class RenderServer;
        friend class ResourceRegistry;
        friend class StorageSet;

        explicit CapabilitySelection(FeatureId implementation) noexcept : implementation_(implementation) {}

        FeatureId implementation_;
    };

    struct GlobalRenderScope final
    {
        bool operator==(const GlobalRenderScope&) const noexcept = default;
    };

    struct ViewRequest final
    {
        SceneId scene;
        ViewId view;
        bool operator==(const ViewRequest&) const noexcept = default;
    };

    // Existing business ownership, not a Storage location or a new lifetime domain.
    using VRenderScope = std::variant<GlobalRenderScope, SceneId, ViewRequest, AttachmentId>;

    // An advertised input of the selected implementation at an installed business scope.
    // The index identifies the implementation input returned by installation, never a physical allocation.
    struct CapabilityInput final
    {
        CapabilitySelection implementation;
        VRenderScope scope;
        std::uint32_t index = 0;
        bool operator==(const CapabilityInput&) const noexcept = default;
    };

    struct RenderCapability final
    {
        std::string name;
        std::uint32_t version = 1;
        std::string description;
        std::vector<RenderPayloadContract> inputs;
        std::vector<std::string> outputs;
        std::vector<rdesc::ETextureFormat> target_formats;
        std::vector<std::uint32_t> target_samples;
        std::vector<ETargetKind> target_kinds;
        bool operator==(const RenderCapability&) const noexcept = default;
    };

    // Portable configuration from a saved project. Bytes are decoded by the selected implementation;
    // they are never reinterpreted as a C++ object. The typed alternative remains available in-process.
    struct SerializedRenderConfiguration final
    {
        std::string schema;
        std::uint32_t version = 1;
        std::vector<std::byte> bytes;
        bool operator==(const SerializedRenderConfiguration&) const noexcept = default;
    };

    using VCapabilityConfiguration = std::variant<ConfigValue, SerializedRenderConfiguration>;

    struct CapabilitySupport final
    {
        CapabilitySelection selection;
        std::string implementation;
        std::uint32_t implementation_version = 1;
        std::string description;
        std::vector<RenderCapability> capabilities;
        RenderPayloadContract configuration;
        std::vector<std::byte> default_configuration;
        RenderError unavailable;
    };

    struct CapabilityQuery final
    {
        std::string capability; // Empty lists all selectable implementations.
        std::uint32_t version = 1;
        std::string implementation;             // Explicit stable selection, never registration-order fallback.
        std::optional<TargetDesc> target;       // Unsupported choices remain visible with an unavailable reason.
        std::vector<std::string> input_schemas; // Required producer contracts, qualified once at selection.
    };

    struct CapabilityList final
    {
        RenderError error;
        std::vector<CapabilitySupport> implementations;
    };

    struct CapabilityInstallation final
    {
        CapabilitySelection selection;
        VRenderScope scope;
        VCapabilityConfiguration configuration;
    };

    struct CapabilitiesInstalled final
    {
        struct Input final
        {
            std::string schema;
            std::uint32_t version = 1;
            CapabilityInput destination;
        };

        RenderError error;
        std::vector<Input> inputs;
    };
} // namespace lux::render
