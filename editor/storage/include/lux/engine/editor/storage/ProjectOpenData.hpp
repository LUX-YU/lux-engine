#pragma once

#include <lux/engine/editor/storage/ProjectPublication.hpp>

namespace lux::editor
{
    struct ProjectOpenData final
    {
        ProjectManifest manifest;
        std::filesystem::path file;
        std::vector<ProjectPackage> mounts;
        ProjectWriteLease write_lease;
        std::string manifest_digest;
        std::vector<std::pair<std::string, std::string>> source_digests;
    };

    // Blocking source preparation, invoked through Process before Project adoption.
    [[nodiscard]] LUX_EDITOR_STORAGE_PUBLIC EditorResult<ProjectOpenData> readProjectOpenData(const std::filesystem::
                                                                                                  path&);

}
