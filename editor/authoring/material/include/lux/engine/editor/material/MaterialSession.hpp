#pragma once

#include <lux/engine/editor/material/MaterialSnapshot.hpp>
#include <lux/engine/editor/sessions/IEditSession.hpp>

namespace lux::editor::material
{
    namespace detail
    {
        struct MaterialSessionAccess;
    }
    struct MaterialSessionLimits final
    {
        editing::HistoryLimits history{1024, 64 * 1024 * 1024, 16 * 1024 * 1024, 256};
    };
    class MaterialPersistenceAccess;
    class MaterialSession final : public sessions::IEditSession
    {
    public:
        [[nodiscard]] static MaterialEditResult<std::unique_ptr<MaterialSession>> create(
            sessions::SessionId id,
            sessions::SourceBinding binding,
            lux::material::MaterialSource source,
            contracts::CodeLease code = contracts::CodeLease::builtin(),
            MaterialSessionLimits limits = {}
        );
        ~MaterialSession() noexcept override;
        MaterialSession(const MaterialSession&) = delete;
        MaterialSession& operator=(const MaterialSession&) = delete;
        [[nodiscard]] sessions::SessionInfo describe() const override;
        // Synchronous history observation; labels are borrowed until the next domain mutation.
        [[nodiscard]] editing::EditResult<editing::HistoryView> historyView() const noexcept;
        [[nodiscard]] MaterialEditResult<MaterialReadView> read() const noexcept;
        [[nodiscard]] MaterialEditResult<MaterialEditReceipt> apply(MaterialEditBatch batch);
        [[nodiscard]] MaterialEditResult<MaterialEditReceipt> undo();
        [[nodiscard]] MaterialEditResult<MaterialEditReceipt> redo();
        [[nodiscard]] MaterialEditResult<MaterialSnapshot> capture(MaterialSnapshotBudget budget = {}) const;

    private:
        friend class MaterialPersistenceAccess;
        friend struct detail::MaterialSessionAccess;
        struct Impl;
        explicit MaterialSession(std::unique_ptr<Impl> impl) noexcept;
        [[nodiscard]] sessions::ContentStamp currentContent() const noexcept override;
        [[nodiscard]] sessions::SessionResult<sessions::ClosePermit> prepareClose(sessions::ContentStamp expected
        ) noexcept override;
        std::unique_ptr<Impl> impl_;
    };
}
