#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/editor/metadata/visibility.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::editor
{
enum class EPluginError : std::uint8_t
{
    IO_FAILURE,
    INVALID_DESCRIPTION,
    DUPLICATE_IDENTITY,
    MISSING_DEPENDENCY,
    DEPENDENCY_VERSION_MISMATCH,
    DEPENDENCY_CYCLE,
    INVALID_PATH,
    LIBRARY_LOAD_FAILURE,
    MISSING_EXPORT,
    INVALID_EXPORT,
    MODULE_MISMATCH,
    ABI_MISMATCH,
    BUILD_MISMATCH,
    DECLARATION_MISMATCH,
    REGISTRATION_FAILURE,
    CANCELLED,
};

struct PluginFailure final
{
    EPluginError code{EPluginError::INVALID_DESCRIPTION};
    std::string plugin;
    std::string subject;
    std::string detail;
};

template <class T> using PluginResult = lux::cxx::expected<T, PluginFailure>;

enum class EPluginExport : std::uint8_t
{
    SIMULATION,
    SCENE,
    COMPONENTS,
    RENDER,
    RENDER_SCENE,
    EDITOR,
};

struct MetadataIdentity final
{
    std::string id;
    std::uint32_t version{};
    friend bool operator==(const MetadataIdentity &, const MetadataIdentity &) = default;
};

struct PluginLibraryDescription final
{
    std::filesystem::path path;
    std::uint32_t interface_version{1};
    std::string sdk_abi;
    std::string build_id;
    std::string declaration_digest;
    std::vector<EPluginExport> exports;
};

struct MetadataField final
{
    std::string id, type, display_name, description, unit, default_display;
    bool read_only{};
    std::optional<double> minimum, maximum;
};

struct MetadataValueType final
{
    MetadataIdentity identity;
    std::string display_name;
    std::vector<MetadataField> fields;
    bool runtime_derived{};
};

struct MetadataAbility final
{
    MetadataIdentity identity;
    std::string display_name, description;
};

struct MetadataImplementation final
{
    MetadataIdentity ability;
    std::string system;
};

struct MetadataRequirement final
{
    std::string slot;
    MetadataIdentity ability;
    bool optional{};
};

struct MetadataAccess final
{
    std::string type;
    bool write{};
    bool external{};
};

enum class EMetadataSystemDomain : std::uint8_t { SIMULATION, SCENE };

struct MetadataSystem final
{
    MetadataIdentity identity;
    EMetadataSystemDomain domain{};
    std::optional<MetadataIdentity> configuration;
    std::vector<MetadataRequirement> requirements;
    std::vector<MetadataAccess> access;
};

struct MetadataFeatureDependency final
{
    MetadataIdentity feature;
    bool optional{};
};

struct MetadataFeature final
{
    MetadataIdentity identity;
    std::string display_name;
    std::optional<MetadataIdentity> configuration;
    bool scene_configurable{};
    bool multiple{};
    std::uint32_t operation_count{};
    std::vector<MetadataFeatureDependency> dependencies;
    std::vector<std::string> conflicts;
};

struct MetadataObservation final
{
    std::string component;
    std::uint8_t events{};
};

struct MetadataRenderBinding final
{
    std::string feature, scene_system;
    std::vector<MetadataObservation> observations;
};

struct PluginDescription final
{
    MetadataIdentity identity;
    std::string author, description;
    bool builtin{};
    std::filesystem::path root;
    PluginLibraryDescription runtime_library;
    std::optional<PluginLibraryDescription> editor_library;
    std::vector<MetadataIdentity> dependencies;
    std::vector<MetadataAbility> abilities;
    std::vector<MetadataImplementation> implementations;
    std::vector<MetadataSystem> systems;
    std::vector<MetadataValueType> components, configurations;
    std::vector<MetadataFeature> render_features;
    std::vector<MetadataRenderBinding> render_scene_bindings;
};

// Owned tool data only. Reading this directory never loads executable code.
class LUX_EDITOR_METADATA_PUBLIC EngineMetadata final
{
  public:
    EngineMetadata() = default;
    EngineMetadata(EngineMetadata &&) noexcept = default;
    EngineMetadata &operator=(EngineMetadata &&) noexcept = default;
    EngineMetadata(const EngineMetadata &) = delete;
    EngineMetadata &operator=(const EngineMetadata &) = delete;
    [[nodiscard]] PluginResult<void> read(const std::filesystem::path &file,
                                          const std::filesystem::path &root) noexcept;
    // Validate the entire addition before publishing; existing record addresses remain stable.
    [[nodiscard]] PluginResult<void> append(EngineMetadata addition) noexcept;
    [[nodiscard]] const PluginDescription *find(std::string_view id) const noexcept;
    [[nodiscard]] const std::deque<PluginDescription> &plugins() const noexcept { return plugins_; }
    [[nodiscard]] PluginResult<std::vector<const PluginDescription *>> loadOrder(std::string_view id) const noexcept;
    [[nodiscard]] std::span<const MetadataImplementation *const> implementations(std::string_view ability) const noexcept;

  private:
    std::deque<PluginDescription> plugins_;
    std::map<std::string, std::vector<const MetadataImplementation *>, std::less<>> implementations_;
};
} // namespace lux::editor
