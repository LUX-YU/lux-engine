#pragma once
#include <lux/engine/editor/detail/ProjectPreparation.hpp>

namespace lux::editor::detail
{
    struct ProjectPrepared final
    {
        std::uint64_t request_serial{};
        execution::TaskId task;
        std::filesystem::path manifest_file;
        ProjectPreparation result;
    };
} // namespace lux::editor::detail
