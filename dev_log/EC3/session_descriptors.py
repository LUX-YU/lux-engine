from pathlib import Path
r=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p=r/'editor/activities/sessions/include/lux/engine/editor/sessions/SessionFactory.hpp';s=p.read_text();s=s.replace('#include <stop_token>','#include <stop_token>\n#include <lux/cxx/core/StableNameId.hpp>');s=s.replace('    // Exact source-format', '    struct SessionKindIdTag;\n    using SessionKindIdView = cxx::StableNameIdView<SessionKindIdTag>;\n\n    // Exact source-format');s=s.replace('std::string canonical_name','std::string_view canonical_name').replace('std::string save_extension','std::string_view save_extension').replace('        SessionKindId kind;\n        std::string label;\n        std::vector<std::string> extensions;','        SessionKindIdView kind;\n        std::string_view label;\n        std::span<const std::string_view> extensions;');s=s.replace('        SessionFactoryEntry(contracts::CodeLease, SessionKindDescriptor, Decode);','''        template <const SessionKindDescriptor& Descriptor>
        [[nodiscard]] static std::shared_ptr<SessionFactoryEntry> bind(contracts::CodeLease code, Decode decode)
        {
            static_assert(Descriptor.kind.isValid() && !Descriptor.label.empty(),
                          "Fixed session metadata must be a valid constant declaration.");
            return std::shared_ptr<SessionFactoryEntry>(
                new SessionFactoryEntry(std::move(code), Descriptor, std::move(decode))
            );
        }
        // Freezes dynamic text and extension spans once; the input is borrowed only during this call.
        [[nodiscard]] static std::shared_ptr<SessionFactoryEntry>
        create(contracts::CodeLease, const SessionKindDescriptor&, Decode);''');s=s.replace('        SessionKindDescriptor descriptor_;','        struct DescriptorStorage;\n        std::unique_ptr<const DescriptorStorage> storage_;\n        const SessionKindDescriptor* descriptor_;');s=s.replace('        friend class SessionLoadJob;','        friend class SessionLoadJob;\n        SessionFactoryEntry(contracts::CodeLease, const SessionKindDescriptor&, Decode);');p.write_text(s)
p=r/'editor/activities/sessions/src/SessionFactory.cpp';s=p.read_text();s=s.replace('SessionFactoryEntry::SessionFactoryEntry(contracts::CodeLease code, SessionKindDescriptor descriptor, Decode decode)','SessionFactoryEntry::SessionFactoryEntry(contracts::CodeLease code, const SessionKindDescriptor& descriptor, Decode decode)').replace('descriptor_(std::move(descriptor))','descriptor_(&descriptor)').replace('return descriptor_;','return *descriptor_;').replace('descriptor_.','descriptor_->');anchor='    SessionFactoryEntry::SessionFactoryEntry';start=s.index(anchor);storage='''    struct SessionFactoryEntry::DescriptorStorage final
    {
        std::string text;
        std::vector<std::string_view> extensions;
        SessionKindDescriptor descriptor;
        explicit DescriptorStorage(const SessionKindDescriptor& input)
        {
            auto size = input.kind.name().size() + input.label.size();
            if (input.source)
                size += input.source->canonical_name.size() + input.source->save_extension.size();
            for (const auto extension : input.extensions)
                size += extension.size();
            text.reserve(size);
            text.append(input.kind.name()).append(input.label);
            for (const auto extension : input.extensions)
                text.append(extension);
            if (input.source)
                text.append(input.source->canonical_name).append(input.source->save_extension);
            const std::string_view bytes{text};
            std::size_t offset{};
            const auto slice = [&](std::size_t count) mutable {
                const auto value = bytes.substr(offset, count);
                offset += count;
                return value;
            };
            auto take = slice;
            descriptor.kind = SessionKindIdView{take(input.kind.name().size())};
            descriptor.label = take(input.label.size());
            extensions.reserve(input.extensions.size());
            for (const auto extension : input.extensions)
                extensions.push_back(take(extension.size()));
            descriptor.extensions = extensions;
            if (input.source)
                descriptor.source = SourceAuthoring{
                    take(input.source->canonical_name.size()), input.source->version,
                    take(input.source->save_extension.size()), input.source->is_default
                };
        }
    };
    std::shared_ptr<SessionFactoryEntry> SessionFactoryEntry::create(
        contracts::CodeLease code, const SessionKindDescriptor& descriptor, Decode decode
    )
    {
        auto storage = std::make_unique<const DescriptorStorage>(descriptor);
        auto entry = std::shared_ptr<SessionFactoryEntry>(
            new SessionFactoryEntry(std::move(code), storage->descriptor, std::move(decode))
        );
        entry->storage_ = std::move(storage);
        return entry;
    }
''';s=s[:start]+storage+s[start:];s=s.replace('descriptor_->kind.name','descriptor_->kind.name()').replace('descriptor().kind == kind','descriptor().kind.name() == kind.name').replace('descriptor_->kind == *preferred','descriptor_->kind.name() == preferred->name');p.write_text(s)
# Module-static metadata remains in the real factory translation unit; source suffixes stay unchanged.
for domain,title,extension,source,suffix in [('scene','Scene','luxscene','lux.scene.package','.scene'),('material','Material','luxmaterial','lux.material.source','.material'),('flow','Flow','luxflow','lux.flowforge.source','.flow')]:
 p=r/f'editor/activities/{domain}/src/SessionFactory.cpp';s=p.read_text();ns={'scene':'scene','material':'material','flow':'flowforge'}[domain];kind=f'lux.editor.{ns}'
 i=s.index('\n{',s.index('namespace lux::editor::'))+2
 s=s[:i]+f'''
    namespace
    {{
        constexpr std::string_view extensions[]{{"{extension}"}};
        constexpr sessions::SessionKindDescriptor descriptor{{
            sessions::SessionKindIdView{{"{kind}"}}, "{title}", extensions,
            sessions::SourceAuthoring{{"{source}", 1, "{suffix}"}}
        }};
    }}'''+s[i:]
 s=s.replace('std::make_shared<SessionFactoryEntry>(', 'SessionFactoryEntry::bind<descriptor>(')
 a=s.index('            SessionKindDescriptor{',s.index('SessionFactoryEntry::bind'));b=s.index('            [',a);s=s[:a]+s[b:];p.write_text(s)
# Explicit owning conversion at actual admission; descriptors never extend a borrowed snapshot.
p=r/'editor/activities/project/src/ProjectAssetSource.cpp';s=p.read_text().replace('(*factory)->descriptor().kind,','sessions::SessionKindId{std::string{(*factory)->descriptor().kind.name()}},');p.write_text(s)
p=r/'editor/application/src/RestoreWorkbench.cpp';s=p.read_text().replace('(*factory)->descriptor().kind, recoveryType', 'sessions::SessionKindId{std::string{(*factory)->descriptor().kind.name()}}, recoveryType');p.write_text(s)
p=r/'editor/activities/sessions/src/ReloadSessionOperation.cpp';s=p.read_text().replace('current->kind != factory->descriptor().kind','current->kind.name != factory->descriptor().kind.name()');p.write_text(s)
p=r/'editor/activities/project/src/ProjectContentSaving.cpp';s=p.read_text().replace('source.canonical_name,','std::string{source.canonical_name},');p.write_text(s)
p=r/'editor/application/src/EditorSaving.cpp';s=p.read_text().replace('std::string("Content/Untitled") + suffix','std::string("Content/Untitled").append(suffix)');p.write_text(s)
# Dynamic consumers still copy once. Test-specific arrays are temporary only inside create().
for p in [r/'editor/tests/integration/session_factories/installation.cpp',r/'cmake/installed-consumers/editor-ec1-skeleton/Model.cpp',r/'cmake/installed-consumers/editor-ec1-skeleton/main.cpp']:
 s=p.read_text().replace('std::make_shared<SessionFactoryEntry>(', 'SessionFactoryEntry::create(').replace('std::make_shared<sessions::SessionFactoryEntry>(', 'sessions::SessionFactoryEntry::create(')
 s=s.replace('descriptor().kind.name','descriptor().kind.name()')
 s=s.replace('{"test.material.alternative"}, "Alternative material", {"material"},','SessionKindIdView{"test.material.alternative"}, "Alternative material",\n                std::array{std::string_view{"material"}},')
 s=s.replace('kind, "Skeleton", {"luxskeleton"},','SessionKindIdView{kind.name}, "Skeleton", std::array{std::string_view{"luxskeleton"}},')
 s=s.replace('{"example.other"}, "Other", {},','sessions::SessionKindIdView{"example.other"}, "Other", {},')
 p.write_text(s)
