#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <algorithm>
#include <fstream>
#include <unordered_set>

namespace lux::editor
{
    EditorResult<ProjectSource> readProjectSource(const std::filesystem::path &file)
    {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(file, error);
        if (error)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "filesystem",
                                                      static_cast<std::uint64_t>(error.value()), error.message()});
        }
        const auto size = std::filesystem::file_size(absolute, error);
        if (error || size > ProjectManifestLimits{}.max_bytes)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "project.read",
                                                      static_cast<std::uint64_t>(error.value()),
                                                      "Project is missing, unreadable or exceeds the size limit"});
        }

        std::ifstream input(absolute, std::ios::binary);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "project.read", 0, "Cannot read the project manifest"});
        }
        auto decoded = decodeProjectManifest(bytes);
        if (!decoded)
        {
            const auto &cause = decoded.error();
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "project.codec",
                                                      static_cast<std::uint64_t>(cause.code), cause.field, cause});
        }

        ProjectSource result{std::move(*decoded), absolute, {}};
        std::unordered_set<std::string> loaded;
        for (const auto &item : result.manifest.assets)
        {
            if (item.cooked_path.empty() || !loaded.insert(item.cooked_path).second)
            {
                continue;
            }
            auto provider = asset::PakAssetProvider::loadFromFile(absolute.parent_path() / item.cooked_path);
            if (!provider)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "project.pak", 0,
                                                          item.cooked_path + ": " + provider.error()});
            }
            const auto root = item.mount_path.empty() ? (*provider)->mountHint() : "/" + item.mount_path;
            result.mounts.push_back({root, std::move(*provider), 0});
        }
        return result;
    }

    Project::Project(object::ObjectDispatcherRef dispatcher) : Object(std::move(dispatcher)) {}

    EditorResult<std::unique_ptr<Project>> Project::open(ProjectSource &source, process::BlockingScheduler blocking,
                                                         object::ObjectDispatcherRef dispatcher)
    {
        if (!blocking || !dispatcher || !dispatcher.isCurrent())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.capabilities"});
        }
        auto result = std::unique_ptr<Project>(new Project(std::move(dispatcher)));
        result->mounts_.reserve(source.mounts.size());
        for (const auto &mount : source.mounts)
        {
            const auto mounted = result->vfs_.mount(mount);
            if (mounted == asset::kInvalidMountId)
            {
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::SOURCE_FAILURE, "project.mount", 0, mount.root});
            }
            result->mounts_.push_back(mounted);
        }
        auto reads = process::asset_loading::VfsAssetReadEndpoint::create(result->vfs_.view(), blocking, {256});
        if (!reads)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::EXECUTION_FAILURE,
                                                      "asset.read.endpoint",
                                                      static_cast<std::uint64_t>(reads.error()),
                                                      {},
                                                      reads.error()});
        }
        result->reads_ = std::move(*reads);
        result->root_ = source.file.parent_path();
        result->source_ = std::move(source);
        result->source_.mounts.clear();
        return result;
    }

    Project::~Project() = default;

    const ProjectAssetEntry *Project::asset(asset::AssetId id) const noexcept
    {
        const auto found = std::ranges::find(source_.manifest.assets, id, &ProjectAssetEntry::id);
        return found == source_.manifest.assets.end() ? nullptr : std::addressof(*found);
    }

    process::asset_loading::AssetReadPort Project::assetReads() const noexcept
    {
        return reads_->port();
    }

    std::string Project::assetName(asset::AssetId id) const
    {
        if (auto path = vfs_.view().pathOf(id))
        {
            return *path;
        }
        if (const auto *item = asset(id))
        {
            return item->mount_path.empty() ? item->source_path : item->mount_path;
        }
        return id.isNull() ? "None" : "Unresolved asset";
    }

    void Project::requestClose() noexcept
    {
        reads_->requestStop();
    }

    EditorResult<bool> Project::advanceClose()
    {
        const auto joined = reads_->join();
        if (!joined)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::EXECUTION_FAILURE, "asset.read.join",
                                                      static_cast<std::uint64_t>(joined.error())});
        }
        for (const auto id : mounts_)
        {
            vfs_.unmount(id);
        }
        mounts_.clear();
        return true;
    }
} // namespace lux::editor
