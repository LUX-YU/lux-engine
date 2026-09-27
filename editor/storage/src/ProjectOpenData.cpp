#include <lux/engine/editor/storage/ProjectOpenData.hpp>
#include <lux/engine/editor/detail/ProjectWrite.hpp>
#include <lux/engine/editor/storage/ProjectPublication.hpp>
#include <lux/engine/editor/PublicationProbe.hpp>
#include <fstream>
#include <unordered_set>
namespace lux::editor
{
    EditorResult<ProjectOpenData> readProjectOpenData(const std::filesystem::path& file)
    {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(file, error);
        if (error)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "filesystem",
                static_cast<std::uint64_t>(error.value()),
                error.message()
            });
        }
        auto lease = ProjectWriteLease::acquire(absolute.parent_path());
        if (!lease)
        {
            return lux::cxx::unexpected(lease.error());
        }
        if (lease->writable())
        {
            auto recovered = recoverProjectFiles(absolute.parent_path());
            if (!recovered)
            {
                return lux::cxx::unexpected(recovered.error());
            }
        }
        else
        {
            const bool publishing = std::filesystem::exists(absolute.parent_path() / ".lux-editor-publication", error);
            if (error)
            {
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::SOURCE_FAILURE,
                    "project.publication",
                    static_cast<std::uint64_t>(error.value()),
                    error.message()
                });
            }
            if (publishing)
            {
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::BUSY,
                    "project.publication",
                    0,
                    "The writer is publishing this project; retry after it reaches a stable state"
                });
            }
        }
        const auto size = std::filesystem::file_size(absolute, error);
        if (error || size > ProjectManifestLimits{}.max_bytes)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "project.read",
                static_cast<std::uint64_t>(error.value()),
                "Project is missing, unreadable or exceeds the size limit"
            });
        }

        std::ifstream input(absolute, std::ios::binary);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "project.read", 0, "Cannot read the project manifest"}
            );
        }
        LUX_EDITOR_IO("manifest-read", absolute, bytes.size());
        auto decoded = decodeProjectManifest(bytes);
        if (!decoded)
        {
            const auto& cause = decoded.error();
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "project.codec",
                static_cast<std::uint64_t>(cause.code),
                cause.field,
                cause
            });
        }

        ProjectOpenData result{std::move(*decoded), absolute, {}};
        result.write_lease = std::move(*lease);
        result.manifest_digest = projectContentDigest(std::as_bytes(std::span(bytes)));
        std::unordered_set<std::string> loaded;
        for (const auto& item : result.manifest.assets)
        {
            auto source_digest = projectFileDigest(absolute.parent_path() / std::filesystem::u8path(item.source_path));
            if (!source_digest)
            {
                return lux::cxx::unexpected(source_digest.error());
            }
            result.source_digests.emplace_back(item.source_path, std::move(*source_digest));
            if (item.cooked_path.empty() || !loaded.insert(item.cooked_path).second)
            {
                continue;
            }
            auto package = readProjectPackage(absolute.parent_path(), item.cooked_path);
            if (!package)
            {
                return lux::cxx::unexpected(package.error());
            }
            if (item.cooked_path != item.source_path)
            {
                auto digest = projectFileDigest(absolute.parent_path() / std::filesystem::u8path(item.cooked_path));
                if (!digest)
                {
                    return lux::cxx::unexpected(digest.error());
                }
                result.source_digests.emplace_back(item.cooked_path, std::move(*digest));
            }
            result.mounts.push_back(std::move(*package));
        }
        return result;
    }

}
