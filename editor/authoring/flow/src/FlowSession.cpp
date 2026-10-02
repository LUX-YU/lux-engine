#include <lux/engine/editor/editing/EditExecutor.hpp>
#include "FlowSessionData.hpp"
#include <algorithm>

namespace lux::editor::flowforge
{
    FlowSession::FlowSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    FlowSession::~FlowSession() noexcept = default;
    FlowEditResult<std::unique_ptr<FlowSession>> FlowSession::create(
        sessions::SessionId id,
        sessions::SourceBinding binding,
        FlowAuthoringSource source,
        lux::flowforge::FlowSourceEnvironment environment,
        FlowSessionLimits limits
    )
    {
        struct Input final
        {
            lux::flowforge::FlowSourceEnvironment environment;
            FlowAuthoringSource source;
        } input{std::move(environment), std::move(source)};
        const bool is_invalid_identity = !id.valid() || (binding && binding->asset != input.source.id);
        if (is_invalid_identity)
            return detail::rejected(EFlowEditError::INVALID_SOURCE);
        auto valid = lux::flowforge::validateFlowSourceEnvironment(input.environment);
        if (!valid)
            return lux::cxx::unexpected(detail::sourceFailure(valid.error()));
        auto captured = lux::flowforge::captureFlowSource(input.source.id, input.source.name, input.source.graph);
        if (!captured)
            return lux::cxx::unexpected(detail::sourceFailure(captured.error()));
        auto graph = lux::flowforge::materializeFlowSource(*captured, input.environment);
        if (!graph)
            return lux::cxx::unexpected(detail::sourceFailure(graph.error()));
        graph->preserveIssuedIdsFrom(input.source.graph);
        auto impl = std::make_unique<Impl>(id, std::move(binding), limits);
        impl->environment = input.environment;
        if (input.environment.code_lifetime)
            impl->code.push_back(contracts::CodeLease::plugin(input.environment.code_lifetime));
        impl->source = {captured->id, captured->name, std::move(*graph)};
        auto history = editing::EditHistory::create({limits.history, {}});
        if (!history)
            return lux::cxx::unexpected(detail::historyFailure(history.error()));
        impl->history = std::move(*history);
        if (impl->state.binding())
            if (auto loaded = impl->state.loaded(impl->content().state); !loaded)
                return lux::cxx::unexpected(FlowEditError{loaded.error()});
        return std::unique_ptr<FlowSession>(new FlowSession(std::move(impl)));
    }
    editing::EditResult<editing::HistoryView> FlowSession::historyView() const noexcept
    {
        return impl_->history->view();
    }
    sessions::SessionInfo FlowSession::describe() const
    {
        const auto content = currentContent();
        return {
            impl_->state.id(),
            {"lux.editor.flowforge"},
            impl_->state.binding(),
            content,
            impl_->state.observed(),
            !impl_->state.checkpoint().clean(content.state, impl_->state.bindingRevision()),
            impl_->state.admission()
        };
    }
    sessions::ContentStamp FlowSession::currentContent() const noexcept
    {
        return impl_->content();
    }
    sessions::SessionResult<sessions::ClosePermit> FlowSession::prepareClose(sessions::ContentStamp expected) noexcept
    {
        return impl_->state.prepareClose(currentContent(), expected);
    }
    FlowEditResult<FlowReadView> FlowSession::read() const noexcept
    {
        if (auto ready = impl_->available(); !ready)
            return lux::cxx::unexpected(ready.error());
        return FlowReadView{impl_->source, currentContent(), impl_->state.observed(), impl_->state.gate()};
    }
    FlowEditResult<FlowEditReceipt> FlowSession::apply(FlowEditBatch batch)
    {
        return impl_->state.gate().withEdit([&](sessions::EditScope&) -> FlowEditResult<FlowEditReceipt> {
            auto edits = std::move(batch.edits); // Cleanup remains admitted on every early return.
            if (batch.expected != currentContent())
                return detail::rejected(EFlowEditError::STALE_CONTENT);
            auto code = impl_->code;
            for (const auto& edit : edits)
                if (const auto* node = std::get_if<FlowInsertNode>(&edit))
                {
                    if (!node->code.valid())
                        return detail::rejected(EFlowEditError::INVALID_SOURCE);
                    if (std::ranges::none_of(code, [&](const auto& present) { return present.sameOwner(node->code); }))
                        code.push_back(node->code);
                }
            auto leases = std::make_shared<const std::vector<contracts::CodeLease>>(code);
            auto prepared = prepareFlowEdit(
                impl_->source,
                impl_->environment,
                currentContent().state,
                std::move(edits),
                std::move(batch.label),
                contracts::CodeLease::plugin(leases),
                {impl_.get(),
                 [](void* raw, const editing::CommitInfo&, bool) noexcept {
                     static_cast<Impl*>(raw)->state.contentChanged();
                 }}
            );
            if (!prepared)
                return lux::cxx::unexpected(detail::historyFailure(prepared.error()));
            auto result = editing::EditExecutor{}.execute(*impl_->history, prepared->operation);
            if (!result)
                return lux::cxx::unexpected(detail::historyFailure(result.error()));
            FlowEditIds inserted;
            if (result->effect == editing::EEditEffect::CHANGE)
            {
                inserted = *prepared->inserted;
                impl_->code.swap(code);
            }
            return FlowEditReceipt{result->effect, currentContent(), impl_->state.observed(), std::move(inserted)};
        });
    }
    FlowEditResult<FlowEditReceipt> FlowSession::Impl::replay(bool forward)
    {
        return state.gate().withEdit([&](sessions::EditScope&) -> FlowEditResult<FlowEditReceipt> {
            auto result = forward ? editing::EditExecutor{}.redo(*history) : editing::EditExecutor{}.undo(*history);
            if (!result)
                return lux::cxx::unexpected(detail::historyFailure(result.error()));
            return FlowEditReceipt{result->effect, content(), state.observed(), {}};
        });
    }
    FlowEditResult<FlowEditReceipt> FlowSession::undo()
    {
        return impl_->replay(false);
    }
    FlowEditResult<FlowEditReceipt> FlowSession::redo()
    {
        return impl_->replay(true);
    }
    FlowEditResult<FlowSnapshot> FlowSession::capture(FlowSnapshotBudget budget) const
    {
        return FlowReadView{impl_->source, currentContent(), impl_->state.observed(), impl_->state.gate()}.capture(
            budget
        );
    }
    FlowEditResult<FlowSnapshot> FlowReadView::captureAdmitted(FlowSnapshotBudget budget) const
    {
        if (detail::sourceBytes(source_) > budget.max_bytes)
            return detail::rejected(EFlowEditError::BUDGET);
        auto source = lux::flowforge::captureFlowSource(source_.id, source_.name, source_.graph);
        if (!source)
            return lux::cxx::unexpected(detail::sourceFailure(source.error()));
        FlowSnapshot result;
        result.source_ = std::move(*source);
        result.content_ = content_;
        result.observed_ = observed_;
        result.retained_bytes_ = detail::sourceBytes(source_);
        return result;
    }
    FlowEditResult<FlowSnapshot> FlowReadView::capture(FlowSnapshotBudget budget) const
    {
        return gate_.withRead([&] { return captureAdmitted(budget); });
    }
    FlowEditResult<std::string> FlowReadView::encode() const
    {
        return withRead([](const lux::flowforge::FlowSource& source) -> FlowEditResult<std::string> {
            auto encoded = lux::flowforge::encodeFlowSource(source);
            if (!encoded)
                return lux::cxx::unexpected(detail::sourceFailure(encoded.error()));
            return std::move(*encoded);
        });
    }
}
