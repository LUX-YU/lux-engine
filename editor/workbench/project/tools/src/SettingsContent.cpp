#include <lux/engine/editor/project/SettingsContent.hpp>
#include <imgui.h>
#include <algorithm>
#include <array>

namespace lux::editor::project
{
    namespace
    {
        template <class Error> auto settingsFailure(std::string domain, const Error& error)
        {
            auto code = EEditorError::SOURCE_FAILURE;
            if constexpr (requires { error.code == decltype(error.code)::BUSY; })
                if (error.code == decltype(error.code)::BUSY)
                    code = EEditorError::BUSY;
            std::string detail;
            if constexpr (requires { error.detail; })
                detail = error.detail;
            return cxx::unexpected(EditorFailure{code, std::move(domain), 0, std::move(detail), error});
        }
        EditorResult<settings::SettingsDocument> read(const SettingsLocation& location)
        {
            auto document = location.store.readSettings(location.relative, location.scope);
            if (document)
                return std::move(*document);
            if (document.error().code != workspace::EWorkspaceError::NOT_FOUND)
                return settingsFailure("settings.read", document.error());
            settings::SettingsDocument empty;
            empty.scope = location.scope;
            return empty;
        }
        void finish(lux::object::LuxObject& node, bool cancel) noexcept
        {
            for (auto* child = node.firstChild(); child; child = child->nextSibling())
                finish(*child, cancel);
            if (auto* element = dynamic_cast<lux::ui::Element*>(&node))
                element->finishEdit(cancel);
        }
    } // namespace
    struct SettingsContent::Impl final
    {
        struct Editing final
        {
            // Destruction order is part of the plugin contract: control, value, then factory code.
            settings::SettingsPage page;
            settings::SettingsDraft draft;
            std::unique_ptr<lux::ui::Element> control;
            struct Publication final
            {
                persistence::WriteTicket ticket;
                std::vector<std::byte> bytes;
            };
            std::optional<Publication> pending;
            Editing(settings::SettingsPage page, settings::SettingsDraft draft)
                : page(std::move(page)), draft(std::move(draft))
            {
            }
        };
        std::shared_ptr<SettingsContentInput> input;
        std::vector<settings::SettingsPage> pages;
        std::unique_ptr<Editing> editing;
        std::optional<EditorFailure> error;
        std::optional<ESettingsAction> action;
        std::optional<std::pair<std::string, settings::ESettingsScope>> selection;
        std::vector<std::string> page_labels;
        std::string selected_label;
        struct ResultAction final
        {
            std::size_t location;
            persistence::WriteTicket ticket;
            bool reconcile;
        };
        std::optional<ResultAction> result_action;
        bool initialized{};
        bool dispatching{};
        struct Dispatch final
        {
            bool& active;
            explicit Dispatch(bool& value) noexcept : active(value)
            {
                active = true;
            }
            ~Dispatch()
            {
                active = false;
            }
            Dispatch(const Dispatch&) = delete;
            Dispatch& operator=(const Dispatch&) = delete;
        };
        explicit Impl(std::shared_ptr<SettingsContentInput> input) : input(std::move(input)) {}

        SettingsLocation* location(settings::ESettingsScope scope)
        {
            auto found = std::ranges::find(input->locations, scope, &SettingsLocation::scope);
            return found == input->locations.end() ? nullptr : &*found;
        }
        EditorResult<void> select(SettingsContent& owner, settings::SettingsIdView id, settings::ESettingsScope scope)
        {
            auto* target = location(scope);
            if (!target)
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "settings.scope"});
            if (editing && editing->pending)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.publication.pending"});
            auto catalog = input->pages();
            const auto found = std::ranges::find_if(
                catalog,
                [&](const auto& page) { return page.entry->descriptor().id.name() == id.name(); }
            );
            if (found == catalog.end())
                return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "settings.registration"});
            std::vector<settings::SettingsDocument> documents;
            std::optional<std::size_t> target_index;
            for (const auto& source : input->locations)
            {
                auto document = read(source);
                if (!document)
                    return cxx::unexpected(document.error());
                if (source.scope == scope)
                    target_index = documents.size();
                documents.push_back(std::move(*document));
            }
            auto resolved = settings::resolveSettings(found->entry, documents);
            if (!resolved)
                return settingsFailure("settings.resolve", resolved.error());
            auto draft = settings::makeSettingsDraft(*resolved, documents[*target_index]);
            if (!draft)
                return settingsFailure("settings.draft", draft.error());
            for (const auto& applied : input->applied)
                if (applied.entry == found->entry)
                    draft->applied = applied.bytes;
            auto candidate = std::make_unique<Editing>(*found, std::move(*draft));
            if (candidate->page.create)
            {
                auto control =
                    candidate->page.create(owner, lux::ui::ElementId{"settings-fields"}, candidate->draft.desired);
                if (!control)
                    return cxx::unexpected(control.error());
                candidate->control = std::move(*control);
            }
            if (editing && editing->control)
                finish(*editing->control, false);
            editing = std::move(candidate);
            pages = std::move(catalog);
            page_labels.clear();
            for (const auto& page : pages)
                page_labels.emplace_back(page.entry->descriptor().label);
            selected_label = editing->page.entry->descriptor().label;
            return {};
        }
        EditorResult<void> request(SettingsContent& owner, ESettingsAction requested)
        {
            if (!editing)
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "settings.no-draft"});
            if (editing->pending)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.publication.pending"});
            if (editing->control)
                finish(*editing->control, false);
            if (requested == ESettingsAction::REVERT)
                return select(owner, editing->page.entry->descriptor().id, editing->draft.scope);
            auto catalog = input->pages();
            const auto entry =
                std::ranges::find_if(catalog, [&](const auto& page) { return page.entry == editing->page.entry; });
            if (entry == catalog.end())
                return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "settings.registration"});
            auto& draft = editing->draft;
            auto* target = location(draft.scope);
            auto source = read(*target);
            if (!source)
                return cxx::unexpected(source.error());
            if (requested == ESettingsAction::DEFAULTS)
            {
                auto defaults = entry->entry->defaults();
                if (!defaults)
                    return settingsFailure("settings.defaults", defaults.error());
                settings::SettingsDraft replacement{
                    draft.entry,
                    draft.scope,
                    draft.based_on,
                    std::move(*defaults),
                    draft.applied,
                    draft.persisted
                };
                auto candidate = std::make_unique<Editing>(editing->page, std::move(replacement));
                if (candidate->page.create)
                {
                    auto created =
                        candidate->page.create(owner, lux::ui::ElementId{"settings-fields"}, candidate->draft.desired);
                    if (!created)
                        return cxx::unexpected(created.error());
                    candidate->control = std::move(*created);
                }
                editing = std::move(candidate);
                return {};
            }
            auto prepared = settings::prepareSettings(draft, *source, *entry->entry);
            if (!prepared)
                return settingsFailure("settings.prepare", prepared.error());
            std::vector<std::byte> bytes;
            auto encoded = draft.desired.encode(bytes);
            if (!encoded)
                return settingsFailure("settings.encode", encoded.error());
            const bool is_restart = entry->entry->descriptor().apply == settings::ESettingsApply::RESTART;
            if (!is_restart)
            {
                auto applied = entry->entry->apply(draft.desired);
                if (!applied)
                    return settingsFailure("settings.apply", applied.error());
                draft.applied = bytes;
                std::erase_if(
                    input->applied,
                    [&](const auto& fact) {
                        return std::ranges::none_of(
                            catalog,
                            [&](const auto& page) { return page.entry == fact.entry; }
                        );
                    }
                );
                auto fact = std::ranges::find(input->applied, draft.entry, &AppliedSetting::entry);
                if (fact == input->applied.end())
                    input->applied.push_back({draft.entry, bytes});
                else
                    fact->bytes = bytes;
            }
            if (requested == ESettingsAction::SAVE)
            {
                // Apply is an extension boundary. A callback can replace the contribution catalog.
                const auto current = input->pages();
                if (std::ranges::none_of(current, [&](const auto& page) { return page.entry == draft.entry; }))
                    return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "settings.registration"});
                if (!target->changes)
                    return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "settings.read-only"});
                auto accepted = target->changes->saveSettings(target->relative, *prepared);
                if (!accepted)
                    return cxx::unexpected(accepted.error());
                editing->pending = Editing::Publication{*accepted, std::move(bytes)};
            }
            else if (is_restart)
                return cxx::unexpected(EditorFailure{
                    EEditorError::INVALID_STATE,
                    "settings.restart-required",
                    0,
                    "Save the desired value and reopen the application to apply it."
                });
            return {};
        }
        void receive()
        {
            if (editing)
                for (const auto& fact : input->applied)
                    if (fact.entry == editing->draft.entry && editing->draft.applied != fact.bytes)
                        editing->draft.applied = fact.bytes;
            if (!editing || !editing->pending)
                return;
            auto& pending = *editing->pending;
            auto* target = location(editing->draft.scope);
            auto results = target->changes->publications();
            const auto found = std::ranges::find(results, pending.ticket, &workspace::WorkspacePublication::ticket);
            if (found == results.end() || !found->result)
                return;
            if (const auto* published = std::get_if<persistence::CommitReceipt>(&*found->result))
            {
                editing->draft.persisted = pending.bytes;
                editing->draft.based_on = published->version;
            }
            else
                error = EditorFailure{
                    EEditorError::SOURCE_FAILURE,
                    "settings.publish",
                    0,
                    "Publication failed. The desired value and last applied value are retained.",
                    *found->result
                };
            // This page has consumed its result. Unknown never arrives as a terminal result here.
            auto acknowledged = target->changes->acknowledge(pending.ticket);
            if (!acknowledged)
            {
                error = acknowledged.error();
                return;
            }
            editing->pending.reset();
        }
    };
    SettingsContent::SettingsContent(
        lux::ui::Element& parent,
        lux::ui::ElementId id,
        std::shared_ptr<SettingsContentInput> input
    )
        : Element(parent, std::move(id)), impl_(std::make_unique<Impl>(std::move(input)))
    {
        setStretch({1, 1});
    }
    SettingsContent::~SettingsContent() = default;
    EditorResult<void> SettingsContent::select(settings::SettingsIdView id, settings::ESettingsScope scope)
    {
        if (!dispatcherRef().isCurrent())
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "settings.owner-thread"});
        if (impl_->dispatching)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.dispatch"});
        const Impl::Dispatch dispatch{impl_->dispatching};
        auto result = impl_->select(*this, id, scope);
        impl_->error = result ? std::optional<EditorFailure>{} : result.error();
        return result;
    }
    EditorResult<void> SettingsContent::request(ESettingsAction action)
    {
        if (!dispatcherRef().isCurrent())
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "settings.owner-thread"});
        if (impl_->dispatching)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.dispatch"});
        const Impl::Dispatch dispatch{impl_->dispatching};
        auto result = impl_->request(*this, action);
        impl_->error = result ? std::optional<EditorFailure>{} : result.error();
        return result;
    }
    const settings::SettingsDraft* SettingsContent::draft() const noexcept
    {
        return impl_->editing ? &impl_->editing->draft : nullptr;
    }
    const std::optional<EditorFailure>& SettingsContent::failure() const noexcept
    {
        return impl_->error;
    }
    void SettingsContent::finishEdit(bool cancel) noexcept
    {
        if (impl_->editing && impl_->editing->control)
            finish(*impl_->editing->control, cancel);
    }
    void SettingsContent::arrangeContent() noexcept
    {
        if (impl_->editing && impl_->editing->control)
            impl_->editing->control->arrange({{}, {rect().size.width, std::max(0.f, rect().size.height - 180.f)}});
    }
    void SettingsContent::update() noexcept
    {
        impl_->receive();
        if (auto action = std::exchange(impl_->result_action, {}))
        {
            auto& changes = *impl_->input->locations[action->location].changes;
            auto result = action->reconcile ? changes.reconcile(action->ticket) : changes.acknowledge(action->ticket);
            if (!result)
                impl_->error = result.error();
        }
        if (!impl_->initialized)
        {
            impl_->pages = impl_->input->pages();
            impl_->initialized = true;
            if (!impl_->pages.empty() && !impl_->input->locations.empty())
                (void)select(impl_->pages.front().entry->descriptor().id, impl_->input->locations.back().scope);
        }
        if (auto selection = std::exchange(impl_->selection, {}))
            (void)select(settings::SettingsIdView{selection->first}, selection->second);
        if (auto action = std::exchange(impl_->action, {}))
            (void)request(*action);
    }
    void SettingsContent::draw() noexcept
    {
        auto& data = *impl_;
        const auto* selected = draft();
        const char* label = selected ? data.selected_label.c_str() : "Choose settings";
        ImGui::BeginDisabled(data.editing && data.editing->pending.has_value());
        if (ImGui::BeginCombo("Page", label))
        {
            for (std::size_t i = 0; i < data.page_labels.size(); ++i)
                if (ImGui::Selectable(data.page_labels[i].c_str()))
                    data.selection = {
                        std::string{data.pages[i].entry->descriptor().id.name()},
                        selected ? selected->scope : data.input->locations.back().scope
                    };
            ImGui::EndCombo();
        }
        if (selected)
        {
            const auto scope_name = [](settings::ESettingsScope scope)
            {
                switch (scope)
                {
                case settings::ESettingsScope::INSTALLATION:
                    return "Installation (read only)";
                case settings::ESettingsScope::PROJECT:
                    return "Project";
                case settings::ESettingsScope::USER:
                    return "User";
                case settings::ESettingsScope::USER_PROJECT:
                    return "This project for this user";
                default:
                    return "Launch override";
                }
            };
            if (ImGui::BeginCombo("Scope", scope_name(selected->scope)))
            {
                for (const auto& location : data.input->locations)
                {
                    if (!(selected->entry->descriptor().scopes & settings::scopeBit(location.scope)))
                        continue;
                    if (ImGui::Selectable(scope_name(location.scope)))
                        data.selection = {std::string{selected->entry->descriptor().id.name()}, location.scope};
                }
                ImGui::EndCombo();
            }
            for (const auto& [text, action] : std::array{
                     std::pair{"Apply", ESettingsAction::APPLY},
                     std::pair{"Save", ESettingsAction::SAVE},
                     std::pair{"Revert", ESettingsAction::REVERT},
                     std::pair{"Defaults", ESettingsAction::DEFAULTS}
                 })
            {
                if (ImGui::Button(text))
                    data.action = action;
                ImGui::SameLine();
            }
            ImGui::NewLine();
            const bool restart = selected->entry->descriptor().apply == settings::ESettingsApply::RESTART;
            ImGui::TextUnformatted(
                restart ? "Saved changes apply after restart." : "Changes apply at this UI safe point."
            );
            if (restart && selected->persisted && selected->persisted != selected->applied)
                ImGui::TextUnformatted("The last saved value is waiting for restart.");
            if (!restart && selected->applied && selected->applied != selected->persisted)
                ImGui::TextUnformatted("The last applied value has not been saved.");
            ImGui::Text(
                "Applied: %s; persisted: %s",
                selected->applied ? "confirmed value" : "not confirmed",
                selected->persisted ? "confirmed value" : "no saved value"
            );
        }
        ImGui::EndDisabled();
        if (ImGui::CollapsingHeader("Accepted settings publications"))
        {
            for (std::size_t i = 0; i < data.input->locations.size(); ++i)
            {
                auto* changes = data.input->locations[i].changes;
                if (!changes)
                    continue;
                ImGui::PushID(static_cast<int>(i));
                for (const auto& report : changes->publications())
                {
                    if (report.refresh_catalog)
                        continue;
                    ImGui::PushID(static_cast<int>(report.ticket.value >> 32));
                    ImGui::PushID(static_cast<int>(report.ticket.value));
                    ImGui::TextUnformatted(report.label.c_str());
                    const bool active =
                        data.editing && data.editing->pending && data.editing->pending->ticket == report.ticket;
                    if (report.result)
                    {
                        ImGui::TextUnformatted(
                            std::holds_alternative<persistence::CommitReceipt>(*report.result)
                                ? "Published"
                                : "Publication failed; original result retained"
                        );
                        if (!active && ImGui::Button("Acknowledge"))
                            data.result_action = Impl::ResultAction{i, report.ticket, false};
                    }
                    else if (ImGui::Button("Reconcile publication"))
                        data.result_action = Impl::ResultAction{i, report.ticket, true};
                    ImGui::PopID();
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
        }
        if (data.error)
            ImGui::TextWrapped("%s: %s", data.error->domain.c_str(), data.error->message.c_str());
        if (data.editing && data.editing->control)
        {
            const float y = ImGui::GetCursorPosY() - rect().position.y;
            ImGui::BeginDisabled(data.editing->pending.has_value());
            drawChild(*data.editing->control, {0, y});
            ImGui::EndDisabled();
        }
    }
} // namespace lux::editor::project
