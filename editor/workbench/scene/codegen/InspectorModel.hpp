#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <nlohmann/json.hpp>
#include <string>

namespace lux::editor::inspector_codegen
{
    using Json = nlohmann::json;
    template <class T> using Result = lux::cxx::expected<T, std::string>;

    // Pure preparation. No files, output fragments, UI dependencies or generated symbol state escape on failure.
    [[nodiscard]] Result<Json> prepare(const Json& meta_unit, const Json& configuration);
} // namespace lux::editor::inspector_codegen
