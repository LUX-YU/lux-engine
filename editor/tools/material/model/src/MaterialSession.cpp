#include "MaterialSessionData.hpp"

namespace lux::editor::material
{
    MaterialSession::MaterialSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    MaterialSession::~MaterialSession() noexcept = default;

    MaterialEditResult<std::unique_ptr<MaterialSession>> MaterialSession::create(
        sessions::SessionId id,
        sessions::SourceBinding binding,
        lux::material::MaterialSource source,
        contracts::CodeLease code,
        MaterialSessionLimits limits
    )
    try
    {
        struct InputRelease final
        {
            lux::material::MaterialSource& source;
            ~InputRelease()
            {
                source.graph = lux::material::MaterialGraph{};
            }
        } release{source}; // Destroy input nodes before the code parameter, also on exceptional exits.
        const bool invalid = !id.valid() || !code.valid() || (binding && binding->asset != source.id);
        if (invalid || !lux::material::validateMaterialSource(source))
            return detail::rejected(EMaterialEditError::INVALID_SOURCE);
        auto impl = std::make_unique<Impl>(id, std::move(binding), limits);
        impl->code.push_back(code);
        // Clone under the supplied code lease, eliminating input aliases before publication.
        impl->source = {source.id, source.name, source.graph.clone()};
        if (!lux::material::validateMaterialSource(impl->source))
            return detail::rejected(EMaterialEditError::CALLBACK);
        auto history = editing::EditHistory::create({limits.history, {}});
        if (!history)
            return lux::cxx::unexpected(detail::historyFailure(history.error()));
        impl->history = std::move(*history);
        if (impl->state.binding())
        {
            auto loaded = impl->state.loaded(impl->content().state);
            if (!loaded)
                return lux::cxx::unexpected(MaterialEditError{loaded.error()});
        }
        return std::unique_ptr<MaterialSession>(new MaterialSession(std::move(impl)));
    }
    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (...)
    {
        return detail::rejected(EMaterialEditError::CALLBACK);
    }

    sessions::SessionInfo MaterialSession::describe() const
    {
        const auto content = currentContent();
        return {
            impl_->state.id(),
            {"lux.editor.material"},
            impl_->state.binding(),
            content,
            impl_->state.observed(),
            !impl_->state.checkpoint().clean(content.state, impl_->state.bindingRevision()),
            impl_->state.admission()
        };
    }
    sessions::ContentStamp MaterialSession::currentContent() const noexcept
    {
        return impl_->content();
    }
    sessions::SessionResult<sessions::ClosePermit> MaterialSession::prepareClose(sessions::ContentStamp expected
    ) noexcept
    {
        return impl_->state.prepareClose(currentContent(), expected);
    }
    MaterialEditResult<MaterialReadView> MaterialSession::read() const noexcept
    {
        if (auto ready = impl_->available(); !ready)
            return lux::cxx::unexpected(ready.error());
        return MaterialReadView{
            impl_->source,
            impl_->code,
            currentContent(),
            impl_->state.observed(),
            impl_->state.gate()
        };
    }
    MaterialEditResult<MaterialEditReceipt> MaterialSession::apply(MaterialEditBatch batch)
    {
        return impl_->state.gate().withEdit([&](sessions::EditScope&) -> MaterialEditResult<MaterialEditReceipt> {
            auto edits = std::move(batch.edits);
            if (batch.expected != currentContent())
                return detail::rejected(EMaterialEditError::STALE_CONTENT);
            auto code = impl_->code;
            const auto retain = [&](const contracts::CodeLease& lease) {
                if (std::ranges::none_of(code, [&](const auto& present) { return present.sameOwner(lease); }))
                    code.push_back(lease);
            };
            for (const auto& edit : edits)
            {
                if (const auto* node = std::get_if<MaterialInsertNode>(&edit))
                    retain(node->code);
                if (const auto* node = std::get_if<MaterialReplaceNode>(&edit))
                    retain(node->code);
            }
            // One immutable lease bundle pins every existing node implementation during replay.
            auto owner = std::make_shared<const std::vector<contracts::CodeLease>>(code);
            auto prepared = prepareMaterialEdit(
                impl_->source,
                currentContent().state,
                std::move(edits),
                std::move(batch.label),
                contracts::CodeLease::plugin(owner),
                {impl_.get(),
                 [](void* raw, const editing::CommitInfo&) noexcept { static_cast<Impl*>(raw)->state.contentChanged(); }
                },
                impl_->limits.history.max_staging_bytes
            );
            if (!prepared)
                return lux::cxx::unexpected(prepared.error());
            auto result = impl_->history->execute(prepared->operation);
            if (!result)
                return lux::cxx::unexpected(detail::historyFailure(result.error()));
            if (result->effect == editing::EEditEffect::CHANGE)
                impl_->code.swap(code);
            return MaterialEditReceipt{
                result->effect,
                currentContent(),
                impl_->state.observed(),
                std::move(prepared->inserted)
            };
        });
    }
    MaterialEditResult<MaterialEditReceipt> MaterialSession::Impl::replay(bool forward)
    {
        return state.gate().withEdit([&](sessions::EditScope&) -> MaterialEditResult<MaterialEditReceipt> {
            auto result = forward ? history->redo() : history->undo();
            if (!result)
                return lux::cxx::unexpected(detail::historyFailure(result.error()));
            return MaterialEditReceipt{result->effect, content(), state.observed(), {}};
        });
    }
    MaterialEditResult<MaterialEditReceipt> MaterialSession::undo()
    {
        return impl_->replay(false);
    }
    MaterialEditResult<MaterialEditReceipt> MaterialSession::redo()
    {
        return impl_->replay(true);
    }
    MaterialEditResult<MaterialSnapshot> MaterialSession::capture(MaterialSnapshotBudget budget) const
    {
        return MaterialReadView{
            impl_->source,
            impl_->code,
            currentContent(),
            impl_->state.observed(),
            impl_->state.gate()
        }
            .capture(budget);
    }
    MaterialEditResult<MaterialSnapshot> MaterialReadView::capture(MaterialSnapshotBudget budget) const
    {
        return gate_.withRead([&]() -> MaterialEditResult<MaterialSnapshot> {
            try
            {
                const auto lease_bytes = code_.size() * sizeof(contracts::CodeLease);
                if (lease_bytes > budget.max_bytes || detail::sourceBytes(source_) > budget.max_bytes - lease_bytes)
                    return detail::rejected(EMaterialEditError::BUDGET);
                MaterialSnapshot result;
                result.code_ = code_;
                result.source_ = {source_.id, source_.name, source_.graph.clone()};
                if (!lux::material::validateMaterialSource(result.source_))
                    return detail::rejected(EMaterialEditError::CALLBACK);
                result.content_ = content_;
                result.observed_ = observed_;
                const auto content_bytes = detail::sourceBytes(result.source_);
                if (content_bytes > budget.max_bytes - lease_bytes)
                    return detail::rejected(EMaterialEditError::BUDGET);
                result.retained_bytes_ = content_bytes + lease_bytes;
                return result;
            }
            catch (const std::bad_alloc&)
            {
                std::terminate();
            }
            catch (...)
            {
                return detail::rejected(EMaterialEditError::CALLBACK);
            }
        });
    }
    MaterialEditResult<std::string> MaterialReadView::encode() const
    {
        return gate_.withRead([&]() -> MaterialEditResult<std::string> {
            auto encoded = lux::material::encodeMaterialSource(source_);
            if (!encoded)
                return detail::rejected(EMaterialEditError::INVALID_SOURCE);
            return std::move(*encoded);
        });
    }
}
