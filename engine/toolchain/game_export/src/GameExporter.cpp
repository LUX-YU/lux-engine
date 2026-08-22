#include <lux/engine/toolchain/game_export/GameExporter.hpp>

#include <lux/engine/authoring/project/Project.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>
#include <lux/engine/resource/asset/AssetHeaderProbe.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/script/ScriptSerDeser.hpp>
#include <lux/engine/scene/SceneAsset.hpp>
#include <lux/engine/scene/SceneAssetSerDeser.hpp>
#include <lux/engine/ecs/scene_format/EntitySection.hpp>
#include <lux/game/LaunchManifest.hpp>
#include <lux/engine/toolchain/asset/cook/PakCook.hpp>
#include <lux/engine/toolchain/asset/texture/TextureImporter.hpp>
#include <lux/engine/toolchain/spatial3d_scene/Spatial3DEntitySceneAdapter.hpp>

#undef TOML_HEADER_ONLY
#define TOML_HEADER_ONLY 1
#undef TOML_EXCEPTIONS
#define TOML_EXCEPTIONS 0
#undef TOML_SHARED_LIB
#define TOML_SHARED_LIB 0
#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace lux::toolchain
{
    namespace
    {
        constexpr std::string_view kCookReceiptName = "game.cook.toml";
        constexpr std::uint32_t kCookReceiptSchema = 2u;

        struct PlatformLayout final
        {
            std::string_view executable_suffix;
            std::string_view module_suffix;
        };

        struct CookedExtension final
        {
            lux::extensions::ExtensionId id;
            std::filesystem::path         source;
            std::uint16_t                 required_major{0u};
            std::uint16_t                 minimum_minor{0u};
        };

        struct CookReceipt final
        {
            std::string                  binary_name;
            std::string                  title;
            std::string                  boot_scene;
            std::filesystem::path        game_pak;
            std::vector<CookedExtension> extensions;
        };

        struct ScopedCookDirectory final
        {
            std::filesystem::path path;

            ScopedCookDirectory() = default;
            explicit ScopedCookDirectory(std::filesystem::path value)
                : path(std::move(value))
            {
            }
            ScopedCookDirectory(const ScopedCookDirectory&) = delete;
            ScopedCookDirectory& operator=(const ScopedCookDirectory&) = delete;
            ScopedCookDirectory(ScopedCookDirectory&& other) noexcept
                : path(std::move(other.path))
            {
                other.path.clear();
            }
            ScopedCookDirectory& operator=(ScopedCookDirectory&& other) noexcept
            {
                if (this == &other)
                    return *this;
                path = std::move(other.path);
                other.path.clear();
                return *this;
            }

            ~ScopedCookDirectory()
            {
                if (path.empty())
                    return;
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        };

        struct AuthoredCookOutput final
        {
            std::vector<PakCookFileEntry> scene_entries;
        };

        struct BuiltinComponentCatalog final
        {
            lux::ecs::ComponentTypeCatalog components;
            std::string error;

            BuiltinComponentCatalog()
            {
                lux::meta::meta_module_init();
                auto registered =
                    lux::ecs::registerGeneratedComponents(components);
                if (!registered)
                {
                    error = "built-in component schema registration failed at '" +
                        registered.error().name + "'";
                }
                else if (*registered == 0u)
                {
                    error = "no built-in component schemas were registered";
                }
            }
        };

        GameExportFailure failure(
            EGameExportError code,
            std::string detail)
        {
            return GameExportFailure{code, std::move(detail)};
        }

        std::string lowerAscii(std::string value)
        {
            std::transform(
                value.begin(),
                value.end(),
                value.begin(),
                [](unsigned char character)
                {
                    return static_cast<char>(std::tolower(character));
                }
            );
            return value;
        }

        bool isSafeBinaryName(std::string_view name) noexcept
        {
            if (name.empty() || name == "." || name == "..")
                return false;
            return std::all_of(
                name.begin(),
                name.end(),
                [](unsigned char character)
                {
                    return std::isalnum(character) != 0 ||
                           character == '_' || character == '-';
                }
            );
        }

        bool hasNativeOnlyCustomization(const GameBinaryRecipe& recipe)
        {
            return !recipe.icon_file.empty() ||
                   !recipe.metadata.version.empty() ||
                   !recipe.metadata.publisher.empty() ||
                   !recipe.metadata.description.empty();
        }

        bool isForbiddenProductFile(std::string_view filename)
        {
            const auto lower = lowerAscii(std::string{filename});
            constexpr std::array forbidden{
                "lux_engine_editor",
                "lux_engine_authoring",
                "lux_engine_toolchain",
                "lux_asset_packer",
                "lux_shader_emitter",
                "lux_game_exporter",
                "imgui-node-editor",
                "node-editor",
                "nativefiledialog",
                "assimp",
                "shaderc",
                "spirv-cross",
                "spirv_cross",
                "flowforge_compiler",
                "toolchain_flowforge",
                "mlir",
                "llvm"
            };
            return std::any_of(
                forbidden.begin(),
                forbidden.end(),
                [&lower](std::string_view token)
                {
                    return lower.find(token) != std::string::npos;
                }
            );
        }

        lux::cxx::expected<PlatformLayout, GameExportFailure> platformLayout(
            ETargetPlatform platform)
        {
            switch (platform)
            {
            case ETargetPlatform::WINDOWS:
                return PlatformLayout{".exe", ".dll"};
            case ETargetPlatform::LINUX:
                return PlatformLayout{"", ".so"};
            case ETargetPlatform::MACOS:
                return PlatformLayout{"", ".dylib"};
            case ETargetPlatform::ANDROID:
                return lux::cxx::unexpected(failure(
                    EGameExportError::TARGET_PLATFORM_UNSUPPORTED,
                    "Android assembly requires the APK packaging adapter; "
                    "desktop file-bundle assembly cannot produce an APK"
                ));
            case ETargetPlatform::UNSPECIFIED:
            default:
                return lux::cxx::unexpected(failure(
                    EGameExportError::INVALID_ARGUMENT,
                    "target platform must be explicit"
                ));
            }
        }

        lux::cxx::expected<void, GameExportFailure> prepareEmptyDirectory(
            const std::filesystem::path& directory,
            std::string_view purpose)
        {
            std::error_code error;
            if (std::filesystem::exists(directory, error))
            {
                if (error || !std::filesystem::is_directory(directory, error) ||
                    error || !std::filesystem::is_empty(directory, error) || error)
                {
                    return lux::cxx::unexpected(failure(
                        EGameExportError::OUTPUT_NOT_EMPTY,
                        std::string{purpose} +
                            " output already exists and is not an empty directory: '" +
                            directory.string() + "'"
                    ));
                }
                return {};
            }

            std::filesystem::create_directories(directory, error);
            if (error)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::FILESYSTEM_ERROR,
                    "cannot create " + std::string{purpose} + " output '" +
                        directory.string() + "': " + error.message()
                ));
            }
            return {};
        }

        lux::cxx::expected<std::vector<std::string>, GameExportFailure>
        readInventory(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream.is_open())
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::RUNTIME_INVENTORY_MISSING,
                    "runtime dependency inventory is missing: '" +
                        path.string() + "'"
                ));
            }

            std::vector<std::string> names;
            std::string line;
            while (std::getline(stream, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty() || line.front() == '#')
                    continue;
                if (std::filesystem::path{line}.filename() != line)
                {
                    return lux::cxx::unexpected(failure(
                        EGameExportError::RUNTIME_FILE_MISSING,
                        "runtime inventory contains a non-filename entry: '" +
                            line + "'"
                    ));
                }
                names.push_back(std::move(line));
            }
            if (!stream.good() && !stream.eof())
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::RUNTIME_INVENTORY_MISSING,
                    "failed while reading runtime dependency inventory: '" +
                        path.string() + "'"
                ));
            }
            std::sort(names.begin(), names.end());
            names.erase(std::unique(names.begin(), names.end()), names.end());
            return names;
        }

        std::optional<std::filesystem::path> findRuntimeFile(
            const std::filesystem::path& runtime_root,
            std::string_view name)
        {
            const std::array candidates{
                runtime_root / "bin" / std::filesystem::path{name},
                runtime_root / std::filesystem::path{name}
            };
            std::error_code error;
            for (const auto& candidate : candidates)
            {
                if (std::filesystem::is_regular_file(candidate, error) && !error)
                    return candidate;
                error.clear();
            }
            return std::nullopt;
        }

        std::optional<std::filesystem::path> findExtensionDependency(
            const std::filesystem::path& module_directory,
            const std::filesystem::path& runtime_root,
            std::string_view name)
        {
            std::error_code error;
            const auto adjacent = module_directory /
                                  std::filesystem::path{name};
            if (std::filesystem::is_regular_file(adjacent, error) && !error)
                return adjacent;
            return findRuntimeFile(runtime_root, name);
        }

        lux::cxx::expected<void, GameExportFailure> copyFile(
            const std::filesystem::path& source,
            const std::filesystem::path& destination)
        {
            std::error_code error;
            std::filesystem::copy_file(
                source,
                destination,
                std::filesystem::copy_options::none,
                error
            );
            if (error)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::FILESYSTEM_ERROR,
                    "cannot copy '" + source.string() + "' to '" +
                        destination.string() + "': " + error.message()
                ));
            }
            return {};
        }

        lux::cxx::expected<std::filesystem::path, GameExportFailure>
        resolveExtensionModule(
            const CookedExtension& extension,
            std::string_view configuration,
            std::string_view module_suffix)
        {
            const auto& source = extension.source;
            std::error_code error;
            if (std::filesystem::is_regular_file(source, error) && !error)
                return source;
            error.clear();
            if (!std::filesystem::is_directory(source, error) || error)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::EXTENSION_INVALID,
                    "runtime extension '" +
                        std::string{extension.id.name()} +
                        "' does not resolve to a file or directory: '" +
                        source.string() + "'"
                ));
            }

            const auto expected_name = std::string{extension.id.name()} +
                                       ".runtime" +
                                       std::string{module_suffix};
            const std::array roots{
                source / std::filesystem::path{configuration},
                source / "bin" / std::filesystem::path{configuration},
                source / "bin",
                source
            };
            for (const auto& root : roots)
            {
                const auto candidate = root / expected_name;
                if (std::filesystem::is_regular_file(candidate, error) && !error)
                    return candidate;
                error.clear();
            }

            return lux::cxx::unexpected(failure(
                EGameExportError::EXTENSION_INVALID,
                "runtime extension '" + std::string{extension.id.name()} +
                    "' is missing target module '" + expected_name + "'"
            ));
        }

        std::string bootSceneVpath(std::string path)
        {
            auto value = std::filesystem::path{std::move(path)};
            value.replace_extension();
            if (!value.empty())
            {
                auto iterator = value.begin();
                if (iterator != value.end() &&
                    iterator->generic_string() == "Worlds")
                {
                    std::filesystem::path scene_path{"Scenes"};
                    ++iterator;
                    for (; iterator != value.end(); ++iterator)
                        scene_path /= *iterator;
                    value = std::move(scene_path);
                }
            }
            return value.generic_string();
        }

        lux::cxx::expected<
            const lux::ecs::ComponentTypeCatalog*,
            GameExportFailure>
        builtinComponentCatalog()
        {
            static BuiltinComponentCatalog state;
            if (!state.error.empty())
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::COOK_FAILED,
                    state.error
                ));
            }
            return &state.components;
        }

        lux::cxx::expected<void, GameExportFailure> writeStagedImage(
            const std::filesystem::path& path,
            std::span<const std::byte> bytes)
        {
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::FILESYSTEM_ERROR,
                    "cannot create staged EntityScene directory '" +
                        path.parent_path().string() + "': " + error.message()
                ));
            }
            auto temporary = path;
            temporary += ".tmp";
            {
                std::ofstream stream(
                    temporary,
                    std::ios::binary | std::ios::trunc
                );
                if (!stream.is_open())
                {
                    return lux::cxx::unexpected(failure(
                        EGameExportError::FILESYSTEM_ERROR,
                        "cannot create staged EntityScene image '" +
                            path.string() + "'"
                    ));
                }
                if (!bytes.empty())
                {
                    stream.write(
                        reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size())
                    );
                }
                stream.flush();
                if (!stream.good())
                {
                    return lux::cxx::unexpected(failure(
                        EGameExportError::FILESYSTEM_ERROR,
                        "cannot write staged EntityScene image '" +
                            path.string() + "'"
                    ));
                }
            }
            std::filesystem::rename(temporary, path, error);
            if (error)
            {
                std::error_code remove_error;
                std::filesystem::remove(temporary, remove_error);
                return lux::cxx::unexpected(failure(
                    EGameExportError::FILESYSTEM_ERROR,
                    "cannot commit staged EntityScene image '" + path.string() +
                        "': " + error.message()
                ));
            }
            return {};
        }

        lux::cxx::expected<std::vector<std::byte>, GameExportFailure>
        readCookInputImage(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::FILESYSTEM_ERROR,
                    "cannot open cooked asset input '" + path.string() + "'"));
            }
            const auto end = stream.tellg();
            if (end <= 0)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::COOK_FAILED,
                    "cooked asset input is empty or unreadable at '" +
                        path.string() + "'"));
            }
            std::vector<std::byte> bytes(static_cast<std::size_t>(end));
            stream.seekg(0);
            stream.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            if (!stream)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::FILESYSTEM_ERROR,
                    "cannot read cooked asset input '" + path.string() + "'"));
            }
            return bytes;
        }

        lux::cxx::expected<void, GameExportFailure> cookEntitySceneDocument(
            const std::filesystem::path& source,
            const std::filesystem::path& source_root,
            const std::filesystem::path& generated_root,
            const lux::ecs::ComponentTypeCatalog& components,
            const Spatial3DMeshAssetCatalog& mesh_assets,
            std::vector<PakCookFileEntry>& entries)
        {
            auto cooked = cookSpatial3DEntitySceneSource(
                source,
                components,
                mesh_assets
            );
            if (!cooked)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::COOK_FAILED,
                    "cannot cook Spatial3D EntityScene from '" +
                        source.string() +
                        "': " + cooked.error().detail));
            }

            std::error_code error;
            auto relative = std::filesystem::relative(
                source,
                source_root,
                error
            );
            if (error || relative.empty() || relative.is_absolute())
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::COOK_FAILED,
                    "cannot derive authored scene relative path for '" +
                        source.string() + "'"
                ));
            }
            for (const auto& part : relative)
            {
                if (part == "." || part == "..")
                {
                    return lux::cxx::unexpected(failure(
                        EGameExportError::COOK_FAILED,
                        "authored scene path escapes Worlds root: '" +
                            source.string() + "'"));
                }
            }
            relative.replace_extension();
            const auto scene_vpath =
                (std::filesystem::path{"Scenes"} / relative).generic_string();
            auto package_path = generated_root / "Scenes" / relative;
            package_path += ".luxscene";
            if (auto written = writeStagedImage(
                    package_path, cooked->encoded_package); !written)
            {
                return written;
            }
            entries.push_back(PakCookFileEntry{
                cooked->package.id,
                lux::scene::kSceneAssetMagic,
                scene_vpath,
                package_path});

            for (const auto& section : cooked->sections)
            {
                const auto key = uuids::to_string(section.record.id.value());
                const auto expected_source =
                    "/Game/EntitySections/" + key;
                const auto* stored = std::get_if<
                    lux::scene::StoredSectionSource>(
                        &section.record.source);
                if (stored == nullptr ||
                    stored->content_path != expected_source)
                {
                    return lux::cxx::unexpected(failure(
                        EGameExportError::COOK_FAILED,
                        "EntitySection '" + key + "' declares source '" +
                            (stored == nullptr
                                ? std::string{"<generated>"}
                                : stored->content_path) +
                            "'; expected absolute Pak path '" +
                            expected_source + "'"));
                }
                const auto section_path = generated_root /
                    "EntitySections" / (key + ".luxentitysection");
                if (auto written = writeStagedImage(
                        section_path, section.encoded_image); !written)
                {
                    return written;
                }
                entries.push_back(PakCookFileEntry{
                    section.record.id.value(),
                    lux::ecs::scene_format::kEntitySectionImageMagic,
                    "EntitySections/" + key,
                    section_path});
            }
            for (const auto& mesh : cooked->generated_meshes)
            {
                auto mesh_path = generated_root / "Content" /
                    std::filesystem::path{mesh.virtual_path};
                mesh_path += ".luxasset";
                if (auto written = writeStagedImage(
                        mesh_path, mesh.encoded_image); !written)
                {
                    return written;
                }
            }
            return {};
        }

        bool isTextureSource(std::string_view extension) noexcept
        {
            constexpr std::array extensions{
                ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds",
                ".ktx2", ".hdr"};
            return std::find(
                extensions.begin(),
                extensions.end(),
                extension) != extensions.end();
        }

        lux::cxx::expected<std::filesystem::path, GameExportFailure>
        generatedDestination(
            const std::filesystem::path& source,
            const std::filesystem::path& source_root,
            const std::filesystem::path& generated_root)
        {
            std::error_code error;
            auto relative = std::filesystem::relative(
                source,
                source_root,
                error
            );
            if (error || relative.empty() || relative.is_absolute())
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::COOK_FAILED,
                    "cannot derive a project-relative path for authored input '" +
                        source.string() + "'"
                ));
            }
            relative.replace_extension(".luxasset");
            const auto destination = generated_root / relative;
            std::filesystem::create_directories(
                destination.parent_path(),
                error
            );
            if (error)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::FILESYSTEM_ERROR,
                    "cannot create generated cook directory '" +
                        destination.parent_path().string() + "': " +
                        error.message()
                ));
            }
            return destination;
        }

        lux::cxx::expected<AuthoredCookOutput, GameExportFailure>
        cookAuthoredSources(
            const std::filesystem::path& content_root,
            const std::filesystem::path& worlds_root,
            const std::filesystem::path& generated_root)
        {
            auto component_catalog = builtinComponentCatalog();
            if (!component_catalog)
            {
                return lux::cxx::unexpected(
                    std::move(component_catalog.error()));
            }
            AuthoredCookOutput output;
            Spatial3DMeshAssetCatalog mesh_assets;
            auto manager = std::make_shared<lux::asset::AssetManager>(
                lux::asset::runtimeAssetCodecCatalog());
            std::error_code error;
            const auto generated_content_root = generated_root / "Content";
            std::filesystem::create_directories(
                generated_content_root,
                error
            );
            if (error)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::FILESYSTEM_ERROR,
                    "cannot create generated cook root '" +
                        generated_root.string() + "': " + error.message()
                ));
            }
            if (std::filesystem::is_directory(content_root, error) && !error)
            {
                for (std::filesystem::recursive_directory_iterator iterator{
                         content_root,
                         error}, end;
                     iterator != end;
                     iterator.increment(error))
                {
                    if (error)
                    {
                        return lux::cxx::unexpected(failure(
                            EGameExportError::FILESYSTEM_ERROR,
                            "cannot enumerate authored Content: " +
                                error.message()
                        ));
                    }
                    if (!iterator->is_regular_file(error))
                    {
                        error.clear();
                        continue;
                    }
                    auto extension = lowerAscii(
                        iterator->path().extension().string());
                    if (extension == ".luxasset" ||
                        extension == ".luxmodel")
                    {
                        const auto header = lux::asset::readAssetHeader(
                            iterator->path());
                        if (extension == ".luxasset" &&
                            header.magic == lux::asset::asset_magic_number_of<
                                lux::asset::EAssetType::MESH>::value)
                        {
                            auto image = readCookInputImage(iterator->path());
                            if (!image)
                            {
                                return lux::cxx::unexpected(
                                    std::move(image.error()));
                            }
                            mesh_assets.meshes.push_back({
                                header.id, std::move(*image)});
                        }
                        continue;
                    }

                    auto destination = generatedDestination(
                        iterator->path(),
                        content_root,
                        generated_content_root
                    );
                    if (!destination)
                        return lux::cxx::unexpected(
                            std::move(destination.error()));

                    if (isTextureSource(extension))
                    {
                        lux::toolchain::TextureImporter importer{manager};
                        auto source_identity = std::filesystem::relative(
                            iterator->path(),
                            content_root,
                            error
                        );
                        if (error)
                        {
                            return lux::cxx::unexpected(failure(
                                EGameExportError::COOK_FAILED,
                                "cannot identify texture source '" +
                                    iterator->path().string() + "'"
                            ));
                        }
                        importer.config().deterministic_seed =
                            "/Game/" + source_identity.generic_string();
                        auto imported = importer.importFromFile(iterator->path());
                        if (!imported || importer.exportAsLuxAsset(
                                imported->second,
                                *destination) !=
                                lux::asset::EAssetError::SUCCESS)
                        {
                            return lux::cxx::unexpected(failure(
                                EGameExportError::COOK_FAILED,
                                "texture cooker failed for '" +
                                    iterator->path().string() + "'"
                            ));
                        }
                        continue;
                    }

                    if (extension == ".lua")
                    {
                        lux::asset::ScriptSerDeser importer{manager};
                        auto source_identity = std::filesystem::relative(
                            iterator->path(),
                            content_root,
                            error
                        );
                        if (error)
                        {
                            return lux::cxx::unexpected(failure(
                                EGameExportError::COOK_FAILED,
                                "cannot identify Lua source '" +
                                    iterator->path().string() + "'"
                            ));
                        }
                        importer.config().deterministic_seed =
                            "/Game/" + source_identity.generic_string();
                        auto imported = importer.importFromFile(iterator->path());
                        if (!imported || importer.exportAsLuxAsset(
                                imported->second,
                                *destination) !=
                                lux::asset::EAssetError::SUCCESS)
                        {
                            return lux::cxx::unexpected(failure(
                                EGameExportError::COOK_FAILED,
                                "Lua cooker failed for '" +
                                    iterator->path().string() + "'"
                            ));
                        }
                        continue;
                    }

                    return lux::cxx::unexpected(failure(
                        EGameExportError::COOK_FAILED,
                        "no offline cooker is registered for authored Content "
                        "file '" + iterator->path().string() + "' (extension '" +
                        extension + "'); import it to a cooked asset first"
                    ));
                }
            }
            error.clear();
            if (std::filesystem::is_directory(worlds_root, error) && !error)
            {
                for (std::filesystem::recursive_directory_iterator iterator{
                         worlds_root,
                         error}, end;
                     iterator != end;
                     iterator.increment(error))
                {
                    if (error)
                    {
                        return lux::cxx::unexpected(failure(
                            EGameExportError::FILESYSTEM_ERROR,
                            "cannot enumerate authored Worlds: " +
                                error.message()
                        ));
                    }
                    if (!iterator->is_regular_file(error))
                    {
                        error.clear();
                        continue;
                    }
                    if (lowerAscii(iterator->path().extension().string()) !=
                        ".luxworld")
                    {
                        // External Actor/Instance/Terrain/Tile/Pixel documents
                        // are reachable only through a validated LXWA root.
                        // They are inputs to that root's cooker, not standalone
                        // World roots and must never become pak entries directly.
                        continue;
                    }
                    if (auto cooked = cookEntitySceneDocument(
                            iterator->path(),
                            worlds_root,
                            generated_root,
                            **component_catalog,
                            mesh_assets,
                            output.scene_entries); !cooked)
                    {
                        return lux::cxx::unexpected(
                            std::move(cooked.error()));
                    }
                }
            }
            return output;
        }

        lux::cxx::expected<void, GameExportFailure> writeCookReceipt(
            const CookReceipt& receipt,
            const std::filesystem::path& path)
        {
            toml::table cook{
                {"schema", static_cast<std::int64_t>(kCookReceiptSchema)},
                {"binary_name", receipt.binary_name},
                {"title", receipt.title},
                {"boot_scene", receipt.boot_scene},
                {"game_pak", receipt.game_pak.generic_string()}};
            toml::table root{{"cook", std::move(cook)}};

            if (!receipt.extensions.empty())
            {
                toml::array extensions;
                for (const auto& extension : receipt.extensions)
                {
                    extensions.push_back(toml::table{
                        {"id", std::string{extension.id.name()}},
                        {"source", extension.source.generic_string()},
                        {"major", static_cast<std::int64_t>(
                                      extension.required_major)},
                        {"minimum_minor", static_cast<std::int64_t>(
                                                extension.minimum_minor)}});
                }
                root.insert("extensions", std::move(extensions));
            }

            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            if (!stream.is_open())
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::MANIFEST_WRITE_FAILED,
                    "cannot create cook receipt '" + path.string() + "'"
                ));
            }
            stream << root << '\n';
            stream.flush();
            if (!stream.good())
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::MANIFEST_WRITE_FAILED,
                    "failed while writing cook receipt '" + path.string() + "'"
                ));
            }
            return {};
        }

        lux::cxx::expected<CookReceipt, GameExportFailure> loadCookReceipt(
            const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream.is_open())
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::COOK_RECEIPT_INVALID,
                    "cook receipt is missing: '" + path.string() + "'"
                ));
            }
            std::ostringstream text;
            text << stream.rdbuf();
            if (!stream.good() && !stream.eof())
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::COOK_RECEIPT_INVALID,
                    "failed while reading cook receipt '" + path.string() + "'"
                ));
            }

            auto parsed = toml::parse(text.str(), path.string());
            if (!parsed)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::COOK_RECEIPT_INVALID,
                    "cook receipt has invalid TOML: '" + path.string() + "'"
                ));
            }
            const auto& root = parsed.table();
            const auto* cook = root["cook"].as_table();
            if (!cook)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::COOK_RECEIPT_INVALID,
                    "cook receipt is missing [cook]: '" + path.string() + "'"
                ));
            }
            const auto schema = (*cook)["schema"].value<std::int64_t>();
            const auto binary_name =
                (*cook)["binary_name"].value<std::string>();
            const auto title = (*cook)["title"].value<std::string>();
            const auto boot_scene =
                (*cook)["boot_scene"].value<std::string>();
            const auto game_pak = (*cook)["game_pak"].value<std::string>();
            if (!schema || *schema != kCookReceiptSchema || !binary_name ||
                !isSafeBinaryName(*binary_name) || !title || !boot_scene ||
                !game_pak || game_pak->empty() ||
                std::filesystem::path{*game_pak}.filename() != *game_pak)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::COOK_RECEIPT_INVALID,
                    "cook receipt contains invalid [cook] fields: '" +
                        path.string() + "'"
                ));
            }

            CookReceipt receipt{
                *binary_name,
                *title,
                *boot_scene,
                std::filesystem::path{*game_pak},
                {}};
            if (const auto* extensions = root["extensions"].as_array())
            {
                receipt.extensions.reserve(extensions->size());
                for (const auto& node : *extensions)
                {
                    const auto* table = node.as_table();
                    if (!table)
                    {
                        return lux::cxx::unexpected(failure(
                            EGameExportError::COOK_RECEIPT_INVALID,
                            "cook receipt contains a non-table extension entry"
                        ));
                    }
                    const auto id = (*table)["id"].value<std::string>();
                    const auto source =
                        (*table)["source"].value<std::string>();
                    const auto major = (*table)["major"].value<std::int64_t>();
                    const auto minor =
                        (*table)["minimum_minor"].value<std::int64_t>();
                    if (!id || !lux::extensions::isCanonicalStableName(*id) ||
                        !source || source->empty() || !major || *major < 0 ||
                        *major > 65535 || !minor || *minor < 0 || *minor > 65535)
                    {
                        return lux::cxx::unexpected(failure(
                            EGameExportError::COOK_RECEIPT_INVALID,
                            "cook receipt contains an invalid extension entry"
                        ));
                    }
                    lux::extensions::ExtensionId parsed_id{*id};
                    for (const auto& existing : receipt.extensions)
                    {
                        if (existing.id.hash() == parsed_id.hash())
                        {
                            return lux::cxx::unexpected(failure(
                                EGameExportError::COOK_RECEIPT_INVALID,
                                "cook receipt repeats or collides at extension '" +
                                    *id + "'"
                            ));
                        }
                    }
                    receipt.extensions.push_back(CookedExtension{
                        std::move(parsed_id),
                        std::filesystem::path{*source},
                        static_cast<std::uint16_t>(*major),
                        static_cast<std::uint16_t>(*minor)});
                }
            }
            return receipt;
        }

        lux::cxx::expected<ScopedCookDirectory, GameExportFailure>
        makeTemporaryCookDirectory()
        {
            static std::atomic_uint64_t serial{0u};
            std::error_code error;
            const auto temp_root = std::filesystem::temp_directory_path(error);
            if (error)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::FILESYSTEM_ERROR,
                    "cannot resolve temporary directory: " + error.message()
                ));
            }

            const auto timestamp = std::chrono::steady_clock::now()
                .time_since_epoch().count();
            for (unsigned attempt = 0u; attempt < 16u; ++attempt)
            {
                const auto candidate = temp_root /
                    ("lux-game-cook-" + std::to_string(timestamp) + "-" +
                     std::to_string(serial.fetch_add(1u)));
                if (std::filesystem::create_directory(candidate, error))
                    return ScopedCookDirectory{candidate};
                if (error && error != std::errc::file_exists)
                {
                    return lux::cxx::unexpected(failure(
                        EGameExportError::FILESYSTEM_ERROR,
                        "cannot create temporary cook directory: " +
                            error.message()
                    ));
                }
                error.clear();
            }
            return lux::cxx::unexpected(failure(
                EGameExportError::FILESYSTEM_ERROR,
                "cannot allocate a unique temporary cook directory"
            ));
        }
    } // namespace

    std::string_view targetPlatformName(ETargetPlatform platform) noexcept
    {
        switch (platform)
        {
        case ETargetPlatform::WINDOWS: return "windows";
        case ETargetPlatform::LINUX: return "linux";
        case ETargetPlatform::MACOS: return "macos";
        case ETargetPlatform::ANDROID: return "android";
        case ETargetPlatform::UNSPECIFIED:
        default: return "unspecified";
        }
    }

    std::optional<ETargetPlatform> parseTargetPlatform(
        std::string_view name) noexcept
    {
        const auto lower = lowerAscii(std::string{name});
        if (lower == "windows" || lower == "win64")
            return ETargetPlatform::WINDOWS;
        if (lower == "linux")
            return ETargetPlatform::LINUX;
        if (lower == "macos" || lower == "mac")
            return ETargetPlatform::MACOS;
        if (lower == "android")
            return ETargetPlatform::ANDROID;
        return std::nullopt;
    }

    lux::cxx::expected<GameCookReport, GameExportFailure>
    cookGame(const GameCookRequest& request) noexcept
    {
        if (request.project_file.empty() || request.output_directory.empty())
        {
            return lux::cxx::unexpected(failure(
                EGameExportError::INVALID_ARGUMENT,
                "project and cooked output directory are required"
            ));
        }

        auto project = lux::authoring::Project::openFromDisk(
            request.project_file);
        if (!project)
        {
            return lux::cxx::unexpected(failure(
                EGameExportError::PROJECT_INVALID,
                std::move(project.error())
            ));
        }

        const auto binary_name = project->manifest().binaryName();
        if (!isSafeBinaryName(binary_name))
        {
            return lux::cxx::unexpected(failure(
                EGameExportError::PROJECT_INVALID,
                "project binary name must contain only letters, digits, '_' or '-'"
            ));
        }
        if (auto prepared = prepareEmptyDirectory(
                request.output_directory,
                "cook");
            !prepared)
        {
            return lux::cxx::unexpected(std::move(prepared.error()));
        }

        const auto game_pak_name = binary_name + ".luxpak";
        const auto game_pak = request.output_directory / game_pak_name;
        const auto generated_root = request.output_directory / ".generated";
        auto transformed = cookAuthoredSources(
            project->contentRoot(),
            project->worldsRoot(),
            generated_root
        );
        if (!transformed)
        {
            return lux::cxx::unexpected(std::move(transformed.error()));
        }
        auto cooked = cookSourcesAndFileEntriesToPak(
            {
                PakCookSource{project->contentRoot(), ""},
                PakCookSource{generated_root / "Content", ""}
            },
            std::move(transformed->scene_entries),
            game_pak,
            "/Game"
        );
        if (!cooked)
        {
            return lux::cxx::unexpected(failure(
                EGameExportError::COOK_FAILED,
                std::move(cooked.error())
            ));
        }

        auto pak_info = lux::asset::inspectPak(game_pak);
        if (!pak_info)
        {
            return lux::cxx::unexpected(failure(
                EGameExportError::COOK_FAILED,
                std::move(pak_info.error())
            ));
        }
        const auto runtime_codecs = lux::scene::makeSceneAssetCodecCatalog(
            *lux::asset::runtimeAssetCodecCatalog());
        if (!runtime_codecs)
        {
            return lux::cxx::unexpected(failure(
                EGameExportError::COOK_FAILED,
                "cannot compose Runtime Scene asset codec catalog"));
        }
        for (const auto& entry : pak_info->entries)
        {
            if (entry.magic_number ==
                    lux::ecs::scene_format::kEntitySectionImageMagic)
                continue;
            const auto* codec = (*runtime_codecs)->findByMagic(
                entry.magic_number);
            if (codec == nullptr ||
                codec->shipping != lux::asset::EAssetShippingClass::RUNTIME)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::AUTHORING_PAYLOAD_PRESENT,
                    "cooked pak contains an asset without a Runtime codec at '" +
                        entry.vpath + "'"
                ));
            }
        }

        CookReceipt receipt{
            binary_name,
            project->manifest().display_name,
            bootSceneVpath(project->manifest().default_world),
            game_pak_name,
            {}};
        for (const auto& extension : project->manifest().extensions)
        {
            if (extension.target !=
                lux::authoring::EProjectExtensionTarget::RUNTIME)
            {
                continue;
            }
            auto source = extension.path.is_absolute()
                ? extension.path
                : project->root() / extension.path;
            receipt.extensions.push_back(CookedExtension{
                lux::extensions::ExtensionId{extension.id.name()},
                source.lexically_normal(),
                extension.required_major,
                extension.minimum_minor});
        }

        const auto receipt_path = request.output_directory /
                                  std::filesystem::path{kCookReceiptName};
        if (auto written = writeCookReceipt(receipt, receipt_path); !written)
            return lux::cxx::unexpected(std::move(written.error()));

        return GameCookReport{
            receipt_path,
            game_pak,
            binary_name,
            cooked->asset_count,
            cooked->payload_bytes};
    }

    lux::cxx::expected<GameAssemblyReport, GameExportFailure>
    assembleGame(const GameAssemblyRequest& request) noexcept
    {
        if (request.cooked_directory.empty() || request.runtime_root.empty() ||
            request.output_directory.empty())
        {
            return lux::cxx::unexpected(failure(
                EGameExportError::INVALID_ARGUMENT,
                "cooked directory, runtime root and assembly output are required"
            ));
        }
        auto layout = platformLayout(request.target_platform);
        if (!layout)
            return lux::cxx::unexpected(std::move(layout.error()));

        if (request.binary.mode == EGameBinaryMode::NATIVE_PROJECT)
        {
            return lux::cxx::unexpected(failure(
                EGameExportError::BINARY_MODE_UNSUPPORTED,
                "native project compile/link is reserved but not implemented; "
                "provide a PREBUILT_BINARY or use the reference Player"
            ));
        }
        if (hasNativeOnlyCustomization(request.binary))
        {
            return lux::cxx::unexpected(failure(
                EGameExportError::BINARY_CUSTOMIZATION_UNSUPPORTED,
                "icon and embedded version metadata require the future native "
                "binary build adapter and cannot be applied to a copied binary"
            ));
        }

        auto receipt = loadCookReceipt(
            request.cooked_directory / std::filesystem::path{kCookReceiptName});
        if (!receipt)
            return lux::cxx::unexpected(std::move(receipt.error()));

        const auto cooked_pak = request.cooked_directory / receipt->game_pak;
        std::error_code error;
        if (!std::filesystem::is_regular_file(cooked_pak, error) || error)
        {
            return lux::cxx::unexpected(failure(
                EGameExportError::COOK_RECEIPT_INVALID,
                "cooked game pak is missing: '" + cooked_pak.string() + "'"
            ));
        }

        if (auto prepared = prepareEmptyDirectory(
                request.output_directory,
                "assembly");
            !prepared)
        {
            return lux::cxx::unexpected(std::move(prepared.error()));
        }

        std::filesystem::path source_binary;
        std::filesystem::path inventory_path;
        if (request.binary.mode == EGameBinaryMode::REFERENCE_PLAYER)
        {
            const auto player_name = std::string{"lux_player"} +
                                     std::string{layout->executable_suffix};
            auto player = findRuntimeFile(request.runtime_root, player_name);
            if (!player)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::RUNTIME_FILE_MISSING,
                    "runtime root does not contain target Player '" +
                        player_name + "'"
                ));
            }
            source_binary = *player;
            inventory_path = request.runtime_root / "bin" /
                             "lux_player.runtime-files";
        }
        else
        {
            if (request.binary.binary_file.empty() ||
                request.binary.runtime_inventory.empty())
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::INVALID_ARGUMENT,
                    "PREBUILT_BINARY requires binary_file and runtime_inventory"
                ));
            }
            if (!std::filesystem::is_regular_file(
                    request.binary.binary_file,
                    error) || error)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::RUNTIME_FILE_MISSING,
                    "prebuilt game binary is missing: '" +
                        request.binary.binary_file.string() + "'"
                ));
            }
            source_binary = request.binary.binary_file;
            inventory_path = request.binary.runtime_inventory;
        }

        auto inventory = readInventory(inventory_path);
        if (!inventory)
            return lux::cxx::unexpected(std::move(inventory.error()));
        for (const auto& dependency : *inventory)
        {
            if (isForbiddenProductFile(dependency))
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::FORBIDDEN_PRODUCT_DEPENDENCY,
                    "runtime closure contains forbidden product file '" +
                        dependency + "'"
                ));
            }
        }

        const auto exported_executable = request.output_directory /
            (receipt->binary_name + std::string{layout->executable_suffix});
        if (auto copied = copyFile(source_binary, exported_executable); !copied)
            return lux::cxx::unexpected(std::move(copied.error()));

        std::set<std::string> deployed_files;
        for (const auto& dependency : *inventory)
        {
            auto source = findExtensionDependency(
                source_binary.parent_path(),
                request.runtime_root,
                dependency
            );
            if (!source)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::RUNTIME_FILE_MISSING,
                    "runtime dependency is missing from runtime root: '" +
                        dependency + "'"
                ));
            }
            if (auto copied = copyFile(
                    *source,
                    request.output_directory / dependency);
                !copied)
            {
                return lux::cxx::unexpected(std::move(copied.error()));
            }
            deployed_files.insert(lowerAscii(dependency));
        }

        const auto exported_pak = request.output_directory / receipt->game_pak;
        if (auto copied = copyFile(cooked_pak, exported_pak); !copied)
            return lux::cxx::unexpected(std::move(copied.error()));

        std::filesystem::create_directories(
            request.output_directory / "extensions",
            error
        );
        if (error)
        {
            return lux::cxx::unexpected(failure(
                EGameExportError::FILESYSTEM_ERROR,
                "cannot create extension output: " + error.message()
            ));
        }

        std::vector<lux::game::ExtensionRequirement>
            exported_extensions;
        for (const auto& extension : receipt->extensions)
        {
            auto module = resolveExtensionModule(
                extension,
                request.configuration,
                layout->module_suffix
            );
            if (!module)
                return lux::cxx::unexpected(std::move(module.error()));
            if (isForbiddenProductFile(module->filename().string()))
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::FORBIDDEN_PRODUCT_DEPENDENCY,
                    "runtime extension resolves to forbidden product file '" +
                        module->filename().string() + "'"
                ));
            }

            const auto relative = std::filesystem::path{"extensions"} /
                                  module->filename();
            if (auto copied = copyFile(
                    *module,
                    request.output_directory / relative);
                !copied)
            {
                return lux::cxx::unexpected(std::move(copied.error()));
            }

            const auto extension_inventory_path = module->parent_path() /
                (module->stem().string() + ".runtime-files");
            auto extension_inventory = readInventory(extension_inventory_path);
            if (!extension_inventory)
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::EXTENSION_INVENTORY_MISSING,
                    "runtime extension '" +
                        std::string{extension.id.name()} +
                        "' is missing dependency inventory: '" +
                        extension_inventory_path.string() + "'"
                ));
            }
            for (const auto& dependency : *extension_inventory)
            {
                if (isForbiddenProductFile(dependency))
                {
                    return lux::cxx::unexpected(failure(
                        EGameExportError::FORBIDDEN_PRODUCT_DEPENDENCY,
                        "runtime extension '" +
                            std::string{extension.id.name()} +
                            "' depends on forbidden product file '" +
                            dependency + "'"
                    ));
                }
                const auto normalized = lowerAscii(dependency);
                if (deployed_files.contains(normalized))
                    continue;
                auto source = findExtensionDependency(
                    module->parent_path(),
                    request.runtime_root,
                    dependency
                );
                if (!source)
                {
                    return lux::cxx::unexpected(failure(
                        EGameExportError::EXTENSION_DEPENDENCY_MISSING,
                        "runtime extension '" +
                            std::string{extension.id.name()} +
                            "' dependency is missing: '" + dependency + "'"
                    ));
                }
                if (auto copied = copyFile(
                        *source,
                        request.output_directory / dependency);
                    !copied)
                {
                    return lux::cxx::unexpected(std::move(copied.error()));
                }
                deployed_files.insert(normalized);
            }
            exported_extensions.push_back(
                lux::game::ExtensionRequirement{
                    extension.id,
                    relative,
                    extension.required_major,
                    extension.minimum_minor}
            );
        }

        std::filesystem::path base_pak_relative;
        const std::array base_pak_candidates{
            request.runtime_root / "share" / "lux-engine" / "engine.luxpak",
            request.runtime_root / "engine.luxpak"
        };
        for (const auto& candidate : base_pak_candidates)
        {
            if (!std::filesystem::is_regular_file(candidate, error) || error)
            {
                error.clear();
                continue;
            }
            base_pak_relative = "base.luxpak";
            if (auto copied = copyFile(
                    candidate,
                    request.output_directory / base_pak_relative);
                !copied)
            {
                return lux::cxx::unexpected(std::move(copied.error()));
            }
            break;
        }

        const auto runtime_manifest = request.output_directory /
            (receipt->binary_name + ".luxruntime.toml");
        lux::game::LaunchManifest manifest;
        manifest.title = request.binary.metadata.display_name.empty()
            ? receipt->title
            : request.binary.metadata.display_name;
        manifest.game_pak = receipt->game_pak;
        manifest.base_pak = base_pak_relative;
        manifest.boot_scene = receipt->boot_scene;
        manifest.extensions = std::move(exported_extensions);
        if (auto written = manifest.saveToFile(runtime_manifest); !written)
        {
            return lux::cxx::unexpected(failure(
                EGameExportError::MANIFEST_WRITE_FAILED,
                std::move(written.error())
            ));
        }

        return GameAssemblyReport{
            exported_executable,
            runtime_manifest,
            exported_pak,
            deployed_files.size(),
            manifest.extensions.size()};
    }

    lux::cxx::expected<GameExportReport, GameExportFailure>
    exportGame(const GameExportRequest& request) noexcept
    {
        auto temporary = makeTemporaryCookDirectory();
        if (!temporary)
            return lux::cxx::unexpected(std::move(temporary.error()));

        auto cooked = cookGame(GameCookRequest{
            request.project_file,
            temporary->path});
        if (!cooked)
            return lux::cxx::unexpected(std::move(cooked.error()));

        auto assembled = assembleGame(GameAssemblyRequest{
            temporary->path,
            request.runtime_root,
            request.output_directory,
            request.target_platform,
            request.configuration,
            request.binary});
        if (!assembled)
            return lux::cxx::unexpected(std::move(assembled.error()));

        return GameExportReport{
            assembled->executable,
            assembled->runtime_manifest,
            assembled->game_pak,
            cooked->asset_count,
            cooked->payload_bytes,
            assembled->runtime_file_count,
            assembled->extension_count};
    }
} // namespace lux::toolchain
