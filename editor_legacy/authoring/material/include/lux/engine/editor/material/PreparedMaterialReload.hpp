#pragma once
#include <lux/engine/editor/material/MaterialSession.hpp>
namespace lux::editor::material
{
    // A complete, unpublished replacement. Preparation may call domain extensions under READING;
    // adoption rechecks the captured content under the original edit gate before the owner swap.
    class PreparedMaterialReload final
    {
    public:
        PreparedMaterialReload(const PreparedMaterialReload&) = delete;
        PreparedMaterialReload& operator=(const PreparedMaterialReload&) = delete;
        PreparedMaterialReload(PreparedMaterialReload&&) noexcept = default;
        PreparedMaterialReload& operator=(PreparedMaterialReload&&) noexcept = default;
        [[nodiscard]] static MaterialEditResult<PreparedMaterialReload> prepare(
            MaterialSession& session,
            lux::material::MaterialSource source,
            lux::object::CodeLease code = lux::object::CodeLease::builtin(),
            std::optional<sessions::ContentStamp> expected = {},
            sessions::SourceBinding binding = {}
        );
        [[nodiscard]] MaterialEditResult<void> adopt(MaterialSession& session);

    private:
        PreparedMaterialReload(sessions::ContentStamp expected, std::unique_ptr<MaterialSession> candidate)
            : expected_(expected), candidate_(std::move(candidate))
        {}
        sessions::ContentStamp expected_;
        std::unique_ptr<MaterialSession> candidate_;
    };
}
