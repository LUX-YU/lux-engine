#include <lux/engine/editor/views/ViewFactory.hpp>

namespace lux::editor::views
{
    struct ViewFactoryInput::Data final
    {
        contracts::CodeLease code;
        object::ObjectDispatcherRef dispatcher;
        lux::ui::PaneId pane;
        cxx::TypeToken type;
        std::shared_ptr<const void> binding;
        std::uint32_t version;
    };
    ViewFactoryInput::ViewFactoryInput(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId pane,
        contracts::CodeLease code,
        cxx::TypeToken type,
        std::shared_ptr<const void> binding,
        std::uint32_t version
    )
        : data_(std::make_shared<Data>(std::move(code), dispatcher, std::move(pane), type, std::move(binding), version))
    {}
    ViewFactoryInput::~ViewFactoryInput()
    {
        const auto code = data_ ? data_->code : contracts::CodeLease::builtin();
        data_.reset();
    }
    ViewFactoryInput& ViewFactoryInput::operator=(ViewFactoryInput other) noexcept
    {
        data_.swap(other.data_);
        return *this;
    }
    object::ObjectDispatcherRef ViewFactoryInput::dispatcher() const noexcept
    {
        return data_->dispatcher;
    }
    const lux::ui::PaneId& ViewFactoryInput::paneId() const noexcept
    {
        return data_->pane;
    }
    const void* ViewFactoryInput::binding() const noexcept
    {
        return data_->binding.get();
    }
    cxx::TypeToken ViewFactoryInput::bindingType() const noexcept
    {
        return data_->type;
    }
    std::uint32_t ViewFactoryInput::version() const noexcept
    {
        return data_->version;
    }
    bool ViewFactoryInput::valid() const noexcept
    {
        return data_ && data_->code.valid() && data_->pane.isValid() && data_->type.isValid() && data_->binding &&
               data_->version;
    }
    ViewFactoryEntry::ViewFactoryEntry(contracts::CodeLease code, ViewFactoryDescriptor descriptor, Create create)
        : code_(std::move(code)), descriptor_(std::move(descriptor)), create_(std::move(create))
    {}
    ViewFactoryEntry::~ViewFactoryEntry() = default;
    const ViewFactoryDescriptor& ViewFactoryEntry::descriptor() const noexcept
    {
        return descriptor_;
    }
    struct ViewFactorySnapshot::Data final
    {
        std::vector<std::shared_ptr<ViewFactoryEntry>> entries;
    };
    ViewFactoryResult<ViewFactorySnapshot> ViewFactorySnapshot::create(
        std::vector<std::shared_ptr<ViewFactoryEntry>> entries,
        std::size_t capacity
    )
    {
        for (auto& entry : entries)
            if (entry)
            {
                auto code = entry->code_;
                entry = contracts::pinCodeOwner(std::move(code), std::move(entry));
            }
        if (entries.size() > capacity)
            return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::CAPACITY, "views"});
        for (std::size_t i{}; i < entries.size(); ++i)
        {
            const auto& entry = entries[i];
            if (!entry)
                return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::INVALID_ARGUMENT, "view.entry"});
            const bool invalid = !entry->code_.valid() || !entry->create_ || !entry->descriptor_.type.isValid() ||
                                 entry->descriptor_.label.empty() || !entry->descriptor_.binding_type.isValid() ||
                                 !entry->descriptor_.input_version;
            if (invalid)
                return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::INVALID_ARGUMENT, "view.descriptor"});
            for (std::size_t j{}; j < i; ++j)
                if (entries[j]->descriptor_.type == entry->descriptor_.type)
                    return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::INVALID_ARGUMENT, "view.duplicate"});
        }
        ViewFactorySnapshot result;
        result.data_ = std::make_shared<Data>(std::move(entries));
        return result;
    }
    ViewFactoryResult<DetachedView> ViewFactorySnapshot::prepare(ViewTypeId type, const ViewFactoryInput& input) const
    {
        // Pin before any callback; releasing the caller's snapshot during create cannot release its code.
        const auto pinned = data_;
        if (!pinned || !input.valid())
            return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::INVALID_ARGUMENT, "view.input"});
        for (const auto& entry : pinned->entries)
        {
            if (entry->descriptor().type != type)
                continue;
            const bool mismatch = input.bindingType() != entry->descriptor().binding_type ||
                                  input.version() != entry->descriptor().input_version;
            if (mismatch)
                return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::INVALID_ARGUMENT, "view.binding"});
            auto invoke = [&]() -> ViewFactoryResult<DetachedView> {
                if (entry->code_.sameOwner(contracts::CodeLease::builtin()))
                    return entry->create_(input);
                try
                {
                    return entry->create_(input);
                }
                catch (const std::bad_alloc&)
                {
                    std::terminate();
                }
                catch (...)
                {
                    return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::CONSTRUCT, "plugin.view.create"});
                }
            };
            auto result = invoke();
            if (!result)
                return result;
            const bool mismatched_output = !result->usesCode(entry->code_) || !result->pane() ||
                                           result->pane()->id() != input.paneId() || result->pane()->type() != type;
            if (mismatched_output)
                return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::CONSTRUCT, "view.output"});
            return result;
        }
        return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::NOT_FOUND, "view.type"});
    }
    std::span<const std::shared_ptr<ViewFactoryEntry>> ViewFactorySnapshot::entries() const noexcept
    {
        return data_ ? std::span<const std::shared_ptr<ViewFactoryEntry>>(data_->entries)
                     : std::span<const std::shared_ptr<ViewFactoryEntry>>{};
    }
}
