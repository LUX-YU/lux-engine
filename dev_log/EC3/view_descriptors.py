from pathlib import Path
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
def edit(p, f):
 p=s/p; a=p.read_text(encoding='utf-8'); b=f(a); assert a!=b,p; p.write_text(b,encoding='utf-8')
edit('editor/editing/include/lux/engine/editor/sessions/SessionId.hpp',lambda a:a.replace('#include <lux/cxx/compile_time/expected.hpp>','#include <lux/cxx/compile_time/expected.hpp>\n#include <lux/cxx/core/StableNameId.hpp>').replace('    struct SessionKindId final','    struct SessionKindIdTag;\n    using SessionKindIdView = cxx::StableNameIdView<SessionKindIdTag>;\n    struct SessionKindId final'))
edit('editor/activities/sessions/include/lux/engine/editor/sessions/SessionFactory.hpp',lambda a:a.replace('#include <lux/cxx/core/StableNameId.hpp>\n','').replace('    struct SessionKindIdTag;\n    using SessionKindIdView = cxx::StableNameIdView<SessionKindIdTag>;\n\n',''))
edit('editor/editing/include/lux/engine/editor/views/ViewInfo.hpp',lambda a:a.replace('    using ViewTypeId =','    using ViewTypeIdView = lux::cxx::StableNameIdView<lux::ui::PaneTypeIdTag>;\n    using ViewTypeId ='))
edit('editor/workbench/desktop/include/lux/engine/editor/views/ViewFactory.hpp',lambda a:a.replace('        BUSY\n','        BUSY,\n        HASH_COLLISION\n').replace('        ViewTypeId type;','        ViewTypeIdView type;').replace('        std::string label;','        std::string_view label;').replace('        std::vector<sessions::SessionKindId> content_kinds;','        std::span<const sessions::SessionKindIdView> content_kinds;').replace('        ViewFactoryEntry(contracts::CodeLease, ViewFactoryDescriptor, Create);','''        template <const ViewFactoryDescriptor& Descriptor>
        [[nodiscard]] static std::shared_ptr<ViewFactoryEntry> bind(contracts::CodeLease code, Create create)
        {
            static_assert(Descriptor.type.isValid() && !Descriptor.label.empty() &&
                          Descriptor.binding_type.isValid() && Descriptor.input_version != 0,
                          "Fixed view metadata must be a valid constant declaration.");
            return std::shared_ptr<ViewFactoryEntry>(
                new ViewFactoryEntry(std::move(code), Descriptor, std::move(create))
            );
        }
        // Freeze dynamic strings/arrays before publishing any borrowed descriptor.
        [[nodiscard]] static std::shared_ptr<ViewFactoryEntry>
        create(contracts::CodeLease, const ViewFactoryDescriptor&, Create);''').replace('        contracts::CodeLease code_;\n        ViewFactoryDescriptor descriptor_;','''        struct DescriptorStorage;
        ViewFactoryEntry(contracts::CodeLease, const ViewFactoryDescriptor&, Create);
        contracts::CodeLease code_;
        std::unique_ptr<const DescriptorStorage> storage_;
        const ViewFactoryDescriptor* descriptor_;'''))
p=s/'editor/workbench/desktop/src/ViewFactory.cpp'; a=p.read_text(); a=a.replace('#include <unordered_map>','#include <algorithm>\n#include <unordered_map>')
begin=a.index('    ViewFactoryEntry::ViewFactoryEntry('); end=a.index('    ViewFactoryResult<ViewFactorySnapshot>',begin)
a=a[:begin]+'''    struct ViewFactoryEntry::DescriptorStorage final
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
            const auto take = [&](std::size_t count) {
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
    ViewFactoryEntry::ViewFactoryEntry(contracts::CodeLease code, const ViewFactoryDescriptor& descriptor, Create create)
        : code_(std::move(code)), descriptor_(&descriptor), create_(std::move(create))
    {}
    std::shared_ptr<ViewFactoryEntry> ViewFactoryEntry::create(
        contracts::CodeLease code, const ViewFactoryDescriptor& descriptor, Create create
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
''' + a[end:]
a=a.replace('entry->descriptor_.','entry->descriptor_->').replace('entries[j]->descriptor_.','entries[j]->descriptor_->').replace('pinned->entries[index]->descriptor_;','pinned->entries[index]->descriptor();').replace('pinned->entries[found->second.front()]->descriptor_.type','pinned->entries[found->second.front()]->descriptor().type')
a=a.replace('            const bool invalid = !entry->code_.valid() || !entry->create_ || !entry->descriptor_->type.isValid() ||\n                                 entry->descriptor_->label.empty() || !entry->descriptor_->binding_type.isValid() ||\n                                 !entry->descriptor_->input_version;\n            if (invalid)','''            const auto& descriptor = entry->descriptor();
            const bool is_invalid_type = !descriptor.type.isValid() ||
                descriptor.type.hash() != cxx::Fnv1a64::hash(descriptor.type.name());
            const bool is_invalid_binding = !entry->code_.valid() || !entry->create_;
            const bool is_invalid_description = descriptor.label.empty() || !descriptor.binding_type.isValid() ||
                !descriptor.input_version;
            const bool is_invalid = is_invalid_type || is_invalid_binding || is_invalid_description;
            if (is_invalid)''')
a=a.replace('                if (kind.name.empty() || !kinds.insert(kind.name).second)','''                const bool is_invalid_kind = !kind.isValid() ||
                    kind.hash() != cxx::Fnv1a64::hash(kind.name()) || !kinds.insert(kind.name()).second;
                if (is_invalid_kind)''').replace('content[kind.name].push_back(i);','content[std::string{kind.name()}].push_back(i);')
old='''            for (std::size_t j{}; j < i; ++j)
                if (entries[j]->descriptor_->type == entry->descriptor_->type)
                    return cxx::unexpected(ViewFactoryFailure{EViewFactoryError::INVALID_ARGUMENT, "view.duplicate"});'''
assert old in a;a=a.replace(old,'')
a=a.replace('        ViewFactorySnapshot result;','''        std::vector<Data::Identity> index;
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
        ViewFactorySnapshot result;''').replace('std::move(entries), std::move(content)','std::move(entries), std::move(index), std::move(content)')
a=a.replace('''        for (const auto& entry : pinned->entries)
        {
            if (entry->descriptor().type != type)
                continue;''','''        if (const auto* found = pinned->find(type.view()))
        {
            const auto& entry = *found;''')
a=a.replace('descriptor.type == *preferred','descriptor.type == preferred->view()').replace('return descriptor.type;','return ViewTypeId{descriptor.type.name()};').replace('selected = descriptor.type;','selected.emplace(descriptor.type.name());').replace('return pinned->entries[found->second.front()]->descriptor().type;','return ViewTypeId{pinned->entries[found->second.front()]->descriptor().type.name()};')
p.write_text(a)
# Migrate every existing dynamic caller; fixed providers move to bind in the next responsibility closure.
for base in ['editor','cmake/installed-consumers','examples']:
 for p in (s/base).rglob('*.cpp'):
  if p.name=='ViewFactory.cpp': continue
  a=p.read_text(encoding='utf-8'); b=a.replace('std::make_shared<views::ViewFactoryEntry>(', 'views::ViewFactoryEntry::create(')
  # Descriptor's first identity only, not unrelated owned values.
  import re
  b=re.sub(r'(views::ViewFactoryDescriptor\{\s*)views::ViewTypeId\{',r'\1views::ViewTypeIdView{',b)
  b=b.replace('ViewFactoryDescriptor{std::move(type),','ViewFactoryDescriptor{type.view(),')
  if b!=a:p.write_text(b,encoding='utf-8')
