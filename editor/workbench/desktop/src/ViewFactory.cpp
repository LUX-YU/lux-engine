#include <lux/engine/editor/views/ViewFactory.hpp>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace lux::editor::views
{
    struct ViewFactoryInput::Data final
    {
        lux::object::CodeLease code;
        object::ObjectDispatcherRef dispatcher;
        lux::ui::PaneId pane;
        cxx::TypeToken type;
        std::shared_ptr<const void> binding;
        std::uint32_t version;
    };
    ViewFactoryInput::ViewFactoryInput(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId pane,
        lux::object::CodeLease code,
        cxx::TypeToken type,
        std::shared_ptr<const void> binding,
        std::uint32_t version
    )
        : data_(std::make_shared<Data>(std::move(code), dispatcher, std::move(pane), type, std::move(binding), version))
    {
    }
    ViewFactoryInput::~ViewFactoryInput()
    {
        const auto code = data_ ? data_->code : lux::object::CodeLease::builtin();
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
    struct ViewFactoryEntry::DescriptorStorage final
    {
        std::string text;
        std::vector<sessions::SessionKindIdView> content_kinds;
        ViewFactoryDescriptor descriptor;
        explicit DescriptorStorage(const ViewFactoryDescriptor& input)
        {
            auto size = input.type.name().size() + input.label.size() + input.binding_type.name().size();
            for (const auto kind : input.content_kinds)
                size += kind.name().size();
            text.reserve(size);
            text.append(input.type.name()).append(input.label).append(input.binding_type.name());
            for (const auto kind : input.content_kinds)
                text.append(kind.name());
            // All growth precedes views. This allocation and its arrays never move after publication.
            const std::string_view bytes{text};
            std::size_t offset{};
            const auto take = [&](std::size_t count)
            {
                const auto value = bytes.substr(offset, count);
                offset += count;
                return value;
            };
            descriptor.type = ViewTypeIdView{take(input.type.name().size())};
            descriptor.label = take(input.label.size());
            descriptor.binding_type = {input.binding_type.hash(), take(input.binding_type.name().size())};
            descriptor.input_version = input.input_version;
            descriptor.default_content_view = input.default_content_view;
            content_kinds.reserve(input.content_kinds.size());
            for (const auto kind : input.content_kinds)
                content_kinds.emplace_back(take(kind.name().size()));
            descriptor.content_kinds = content_kinds;
        }
    };
    ViewFactoryEntry::ViewFactoryEntry(
        lux::object::CodeLease code,
        const ViewFactoryDescriptor& descriptor,
        Create create
    )
        : code_(std::move(code)), descriptor_(&descriptor), create_(std::move(create))
    {
    }
    std::shared_ptr<ViewFactoryEntry> ViewFactoryEntry::create(
        lux::object::CodeLease code,
        const ViewFactoryDescriptor& descriptor,
        Create create
    )
    {
        auto storage = std::make_unique<const DescriptorStorage>(descriptor);
        auto entry = std::shared_ptr<ViewFactoryEntry>(
            new ViewFactoryEntry(std::move(code), storage->descriptor, std::move(create))
        );
        entry->storage_ = std::move(storage);
        return entry;
    }
    ViewFactoryEntry::~ViewFactoryEntry() = default;
    const ViewFactoryDescriptor& ViewFactoryEntry::descriptor() const noexcept
    {
        return *descriptor_;
    }
    struct ViewFactorySnapshot::Data final
    {
        struct Identity final
        {
            std::uint64_t hash;
            std::size_t entry;
        };
        std::vector<std::shared_ptr<ViewFactoryEntry>> entries;
        std::vector<Identity> index;
        std::unordered_map<std::string, std::vector<std::size_t>> content;
        [[nodiscard]] const std::shared_ptr<ViewFactoryEntry>* find(ViewTypeIdView type) const noexcept
        {
            const auto found = std::ranges::lower_bound(index, type.hash(), {}, &Identity::hash);
            if (found == index.end() || found->hash != type.hash())
                return nullptr;
            const auto& value = entries[found->entry];
            // Public text/type resolution is cold; a colliding external name cannot select a factory.
            return value->descriptor().type.name() == type.name() ? &value : nullptr;
        }
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
                entry = lux::object::pinCodeOwner(std::move(code), std::move(entry));
            }
        if (entries.size() > capacity)
            return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::CAPACITY, "views"});
        std::unordered_map<std::string, std::vector<std::size_t>> content;
        for (std::size_t i{}; i < entries.size(); ++i)
        {
            const auto& entry = entries[i];
            if (!entry)
                return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::INVALID_ARGUMENT, "view.entry"});
            const auto& descriptor = entry->descriptor();
            const bool is_invalid_type =
                !descriptor.type.isValid() || descriptor.type.hash() != cxx::Fnv1a64::hash(descriptor.type.name());
            const bool is_invalid_binding = !entry->code_.valid() || !entry->create_;
            const bool is_invalid_description =
                descriptor.label.empty() || !descriptor.binding_type.isValid() || !descriptor.input_version;
            const bool is_invalid = is_invalid_type || is_invalid_binding || is_invalid_description;
            if (is_invalid)
                return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::INVALID_ARGUMENT, "view.descriptor"});
            std::unordered_set<std::string_view> kinds;
            for (const auto& kind : entry->descriptor_->content_kinds)
            {
                const bool is_invalid_kind = !kind.isValid() || kind.hash() != cxx::Fnv1a64::hash(kind.name()) ||
                                             !kinds.insert(kind.name()).second;
                if (is_invalid_kind)
                {
                    return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::INVALID_ARGUMENT, "view.content.kind"}
                    );
                }
                content[std::string{kind.name()}].push_back(i);
            }
        }
        std::vector<Data::Identity> index;
        index.reserve(entries.size());
        for (std::size_t i{}; i < entries.size(); ++i)
            index.push_back({entries[i]->descriptor().type.hash(), i});
        std::ranges::sort(index, {}, &Data::Identity::hash);
        for (std::size_t i = 1; i < index.size(); ++i)
        {
            if (index[i - 1].hash != index[i].hash)
                continue;
            const bool is_duplicate = entries[index[i - 1].entry]->descriptor().type.name() ==
                                      entries[index[i].entry]->descriptor().type.name();
            return cxx::unexpected(ViewFactoryFailure{
                is_duplicate ? EViewFactoryError::INVALID_ARGUMENT : EViewFactoryError::HASH_COLLISION,
                is_duplicate ? "view.duplicate" : "view.identity.collision"
            });
        }
        ViewFactorySnapshot result;
        result.data_ = std::make_shared<Data>(std::move(entries), std::move(index), std::move(content));
        return result;
    }
    ViewFactoryResult<DetachedView> ViewFactorySnapshot::prepare(ViewTypeId type, const ViewFactoryInput& input) const
    {
        // Pin before any callback; releasing the caller's snapshot during create cannot release its code.
        const auto pinned = data_;
        if (!pinned || !input.valid())
            return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::INVALID_ARGUMENT, "view.input"});
        if (const auto* found = pinned->find(type.view()))
        {
            const auto& entry = *found;
            const bool mismatch = input.bindingType() != entry->descriptor().binding_type ||
                                  input.version() != entry->descriptor().input_version;
            if (mismatch)
                return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::INVALID_ARGUMENT, "view.binding"});
            auto invoke = [&]() -> ViewFactoryResult<DetachedView>
            {
                if (entry->code_.sameOwner(lux::object::CodeLease::builtin()))
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
    ViewFactoryResult<ViewTypeId> ViewFactorySnapshot::selectContent(
        const sessions::SessionKindId& kind,
        std::optional<ViewTypeId> preferred
    ) const
    {
        const auto pinned = data_;
        if (!pinned)
        {
            return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::NOT_FOUND, "view.content"});
        }
        const auto found = pinned->content.find(kind.name);
        if (found == pinned->content.end())
        {
            return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::NOT_FOUND, "view.content"});
        }
        std::optional<ViewTypeId> selected;
        std::size_t defaults{};
        std::string candidates;
        for (const auto index : found->second)
        {
            const auto& descriptor = pinned->entries[index]->descriptor();
            if (preferred && descriptor.type == preferred->view())
            {
                return ViewTypeId{descriptor.type.name()};
            }
            if (descriptor.default_content_view)
            {
                ++defaults;
                selected.emplace(descriptor.type.name());
            }
            if (!candidates.empty())
            {
                candidates += ", ";
            }
            candidates += descriptor.type.name();
        }
        if (preferred)
        {
            return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::NOT_FOUND, "view.content"});
        }
        if (defaults == 1)
        {
            return *selected;
        }
        if (found->second.size() == 1)
        {
            return ViewTypeId{pinned->entries[found->second.front()]->descriptor().type.name()};
        }
        return cxx::unexpected(
            ViewFactoryFailure{EViewFactoryError::AMBIGUOUS, "view.content", 0, std::move(candidates)}
        );
    }
} // namespace lux::editor::views
