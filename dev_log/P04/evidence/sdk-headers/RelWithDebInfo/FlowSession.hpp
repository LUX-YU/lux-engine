#pragma once
#include <lux/engine/editor/flowforge/FlowSnapshot.hpp>
#include <lux/engine/editor/sessions/IEditSession.hpp>

namespace lux::editor::flowforge
{
    namespace detail
    {
        struct FlowSessionAccess;
        class PreparedFlowReload;
    }
    struct FlowSessionLimits final
    {
        editing::HistoryLimits history{1024, 64 * 1024 * 1024, 16 * 1024 * 1024, 256};
    };
    class FlowSession final : public sessions::IEditSession
    {
    public:
        // Dynamic environment.code_lifetime owns the span arrays, descriptors and their code.
        // A static environment may omit it. No external graph/Node pointer remains authoritative.
        [[nodiscard]] static FlowEditResult<std::unique_ptr<FlowSession>> create(
            sessions::SessionId id,
            sessions::SourceBinding binding,
            FlowAuthoringSource source,
            lux::flowforge::FlowSourceEnvironment environment = {},
            FlowSessionLimits limits = {}
        );
        ~FlowSession() noexcept override;
        FlowSession(const FlowSession&) = delete;
        FlowSession& operator=(const FlowSession&) = delete;
        [[nodiscard]] sessions::SessionInfo describe() const override;
        [[nodiscard]] FlowEditResult<FlowReadView> read() const noexcept;
        [[nodiscard]] FlowEditResult<FlowEditReceipt> apply(FlowEditBatch batch);
        [[nodiscard]] FlowEditResult<FlowEditReceipt> undo();
        [[nodiscard]] FlowEditResult<FlowEditReceipt> redo();
        [[nodiscard]] FlowEditResult<FlowSnapshot> capture(FlowSnapshotBudget budget = {}) const;

    private:
        friend struct detail::FlowSessionAccess;
        friend class detail::PreparedFlowReload;
        struct Impl;
        explicit FlowSession(std::unique_ptr<Impl> impl) noexcept;
        [[nodiscard]] sessions::ContentStamp currentContent() const noexcept override;
        [[nodiscard]] sessions::SessionResult<sessions::ClosePermit> prepareClose(sessions::ContentStamp expected
        ) noexcept override;
        std::unique_ptr<Impl> impl_;
    };
}
