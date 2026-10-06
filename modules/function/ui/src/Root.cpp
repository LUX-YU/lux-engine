#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Command.hpp>
#include <lux/engine/ui/detail/ContextActivation.hpp>
#include <lux/engine/ui/detail/Context.hpp>
#include <lux/engine/ui/detail/Contract.hpp>
#include <lux/engine/ui/detail/AttachmentState.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <optional>
#include <utility>
#include <map>
#include <set>

namespace lux::ui
{
    namespace
    {
        template<class Visit> void visitSubtree(object::LuxObject& root, Visit&& visit) noexcept
        {
            auto* node = &root;
            for (;;)
            {
                visit(*node);
                if (node->firstChild())
                    node = node->firstChild();
                else
                {
                    while (node != &root && !node->nextSibling())
                        node = node->parent();
                    if (node == &root)
                        return;
                    node = node->nextSibling();
                }
            }
        }

    } // namespace

    struct PreparedDockTree::Data final
    {
        DockTree tree;
        std::vector<std::uint32_t> order;
        std::vector<ImGuiID> ids;
    };
    PreparedDockTree::PreparedDockTree(std::unique_ptr<Data> data) noexcept : data_(std::move(data)) {}
    PreparedDockTree::~PreparedDockTree() = default;
    PreparedDockTree::PreparedDockTree(PreparedDockTree&&) noexcept = default;
    PreparedDockTree& PreparedDockTree::operator=(PreparedDockTree&&) noexcept = default;

    struct Root::Impl final
    {
        void queueChange(object::LuxObject&, ChangeCallback) noexcept;
        void cancelChanges(object::LuxObject&) noexcept;
        void applyPendingChanges() noexcept;
        void drawMenu(Root&) noexcept;
        void drawMenuItems(Root&, std::span<const MenuItem>) noexcept;
        void menuCommand(Root&, Command&, std::size_t) noexcept;
        bool shortcut(Root&, const Key&) noexcept;
        std::shared_ptr<const void> menu_source;
        std::vector<MenuItem> menu;
        Pane* menu_pane{};
        Element* menu_element{};
        bool menu_open{};
        struct MenuCall final
        {
            object::LuxObject* target{};
            CommandId command;
        };
        std::vector<MenuCall> menu_calls;
        bool reset_docking{};
        float menu_height{};
        std::uint64_t window_revision{};

        std::unique_ptr<detail::Context> context;
        window::LuxWindow* window{};
        struct Target final
        {
            Pane* window{};
            Element* element{};
            Target() noexcept = default;
            Target(Pane* value) noexcept : window(value) {}
            Target(Element* value) noexcept : element(value) {}
            Target(std::nullptr_t) noexcept {}
            [[nodiscard]] object::LuxObject* object() const noexcept
            {
                return element ? static_cast<object::LuxObject*>(element) : window;
            }
            [[nodiscard]] Pane* pane() const noexcept
            {
                return element ? &element->pane() : window;
            }
            [[nodiscard]] bool visible() const noexcept
            {
                return element ? element->displayed() : window && window->visible();
            }
            explicit operator bool() const noexcept
            {
                return object() != nullptr;
            }
            [[nodiscard]] object::LuxObject& operator*() const noexcept
            {
                return *object();
            }
            bool operator==(const Target&) const noexcept = default;
        };
        struct Entry final
        {
            object::LuxObject* object{};
            void (*update)(object::LuxObject*) noexcept {};
            std::size_t* registration_slot{};
        };
        Pane *focused{}, *hovered{}, *pending_focus{};
        Target pointer_capture;
        Pane *draw_focused{}, *draw_hovered{};
        Element *focused_element{}, *hovered_element{}, *pending_element{};
        Element *draw_focused_element{}, *draw_hovered_element{};
        detail::AttachmentState* preparation{};
        std::uint64_t structure_revision{};
        std::size_t attachment_capacity{65536};
        std::vector<Entry> registrations;
        std::vector<Pane*> windows;
        struct Change final
        {
            object::LuxObject* target{};
            ChangeCallback apply{};
            bool operator==(const Change&) const noexcept = default;
        };
        std::vector<Change> changes;
        std::size_t change_batch_size{};
        object::LuxObject* active_change{};
        object::LuxObject* active_update{};
        std::size_t layout_depth{};
        std::uint64_t layout_epoch{};
        bool registration_holes{}, window_holes{};
        bool committing_structure{};
        ImGuiKeyChord routed_modifiers{};
        bool drawing{}, updating{}, docking{}, dock_layout_initialized{};
        bool composing{};
        Pane* modal{};
        std::optional<DockLayout> split_layout;
        std::unique_ptr<PreparedDockTree::Data> pending_dock_tree;
        std::array<ImGuiID, 5> dock_regions{};
        struct Placement final
        {
            std::string_view id;
            ImVec2 position, size;
        };
        std::array<Placement, 5> placements{};
        void compactRegistrations() noexcept
        {
            if (std::exchange(registration_holes, false))
            {
                std::size_t kept{};
                for (const auto entry : registrations)
                    if (entry.object)
                    {
                        *entry.registration_slot = kept;
                        registrations[kept++] = entry;
                    }
                registrations.resize(kept);
            }
            if (std::exchange(window_holes, false))
            {
                std::size_t kept{};
                for (auto* pane : windows)
                    if (pane)
                    {
                        pane->window_slot_ = kept;
                        windows[kept++] = pane;
                    }
                windows.resize(kept);
            }
        }
    };

    Root::Root() noexcept : LuxObject() {}
    Root::~Root() noexcept
    {
        if (!isOnAffinityThread())
            detail::failContract();
        if (impl_)
        {
            if (impl_->preparation)
                impl_->preparation->root = nullptr;
            checkDestruction(*this);
            checkContentChange();
            beginDestruction();
            releaseSubtree(*this, false);
            prepareChildrenRelease(*this);
            clearChildren();
        }
    }

    Root::CreateResult Root::create(RootConfig config) noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent())
            return lux::cxx::unexpected(EInitError::WRONG_THREAD);
        {
            auto root = std::unique_ptr<Root>(new Root());
            auto initialized = root->initialize(config);
            if (!initialized)
                return lux::cxx::unexpected(initialized.error());
            return root;
        }
    }

    lux::cxx::expected<void, EInitError> Root::initialize(RootConfig config) noexcept
    {
        if (!isOnAffinityThread())
            return cxx::unexpected(EInitError::WRONG_THREAD);
        if (impl_)
            detail::failContract();
        auto context = detail::Context::create(config);
        if (!context)
            return cxx::unexpected(context.error());
        auto data = std::make_unique<Impl>();
        data->context = std::move(*context);
        data->attachment_capacity = config.attachment_capacity;
        data->docking = config.docking;
        impl_ = std::move(data);
        return {};
    }

    void Root::requireOwner() const noexcept
    {
        if (!impl_ || !isOnAffinityThread())
            detail::failContract();
    }
    const Theme& Root::theme() const noexcept
    {
        requireOwner();
        return impl_->context->theme();
    }
    float Root::scale() const noexcept
    {
        requireOwner();
        return impl_->context->scale();
    }
    lux::cxx::expected<FontAtlas, EInitError> Root::fontAtlas() const noexcept
    {
        requireOwner();
        return impl_->context->fontAtlas();
    }
    cxx::expected<PaneHandle, EAttachmentError> Root::identify(const Pane& pane) const noexcept
    {
        const bool is_wrong_thread = !isOnAffinityThread() || !pane.isOnAffinityThread();
        if (is_wrong_thread)
        {
            return cxx::unexpected(EAttachmentError::WRONG_THREAD);
        }
        if (isClosing())
        {
            return cxx::unexpected(EAttachmentError::CLOSED);
        }
        if (pane.attachedRoot() != this)
        {
            return cxx::unexpected(EAttachmentError::NOT_ATTACHED);
        }
        auto reference = pane.objectId();
        if (!object::ObjectRuntime::instance().resolve(reference))
        {
            return cxx::unexpected(EAttachmentError::CLOSED);
        }
        PaneHandle handle;
        handle.pane_ = std::move(reference);
        handle.root_ = objectId();
        handle.attachment_ = pane.attachment_epoch_;
        return handle;
    }

    cxx::expected<Pane*, EAttachmentError> Root::findPane(const PaneHandle& handle) const noexcept
    {
        if (!isOnAffinityThread())
        {
            return cxx::unexpected(EAttachmentError::WRONG_THREAD);
        }
        if (isClosing())
        {
            return cxx::unexpected(EAttachmentError::CLOSED);
        }
        const bool is_wrong_root = !handle.valid() || handle.root_ != objectId();
        if (is_wrong_root)
        {
            return cxx::unexpected(EAttachmentError::NOT_ATTACHED);
        }
        auto resolved = object::ObjectRuntime::instance().resolve(handle.pane_);
        if (!resolved)
        {
            return cxx::unexpected(EAttachmentError::NOT_ATTACHED);
        }
        // Only identify() can create this handle, from a real Pane on the same Root thread.
        auto* pane = static_cast<Pane*>(*resolved);
        const bool is_stale_attachment = pane->root_ != this || pane->attachment_epoch_ != handle.attachment_;
        if (is_stale_attachment)
        {
            return cxx::unexpected(EAttachmentError::NOT_ATTACHED);
        }
        return pane;
    }

    cxx::expected<void, EAttachmentError>
    Root::withPane(const PaneHandle& handle, cxx::function_ref<void(Pane&)> visit) noexcept
    {
        auto resolved = findPane(handle);
        if (!resolved)
        {
            return cxx::unexpected(resolved.error());
        }
        if (!attachmentSafe())
        {
            return cxx::unexpected(EAttachmentError::BUSY);
        }
        auto& pane = **resolved;
        beginCallbackBorrow(*this);
        beginCallbackBorrow(pane);
        impl_->active_change = &pane;
        visit(pane);
        impl_->active_change = nullptr;
        endCallbackBorrow(pane);
        endCallbackBorrow(*this);
        return {};
    }

    Pane* Root::findPane(PaneIdView id) const noexcept
    {
        requireOwner();
        Pane* result{};
        std::size_t matches{};
        for (auto* pane : impl_->windows)
            if (pane && pane->id().view() == id)
            {
                result = pane;
                ++matches;
            }
        return matches == 1 ? result : nullptr;
    }
    void Root::showPanes() noexcept
    {
        requireOwner();
        beginTreeVisit();
        for (auto* pane : impl_->windows)
            if (pane && !pane->modal())
                pane->setVisible(true);
        endTreeVisit();
    }
    Pane* Root::focusedPane() const noexcept
    {
        requireOwner();
        return impl_->focused;
    }
    bool Root::requestFocus(Pane& pane) noexcept
    {
        requireOwner();
        if (pane.attachedRoot() != this || !pane.visible() || !allowedByModal(pane))
            return false;
        impl_->pending_focus = &pane;
        return true;
    }
    bool Root::requestFocus(PaneIdView id) noexcept
    {
        auto* pane = findPane(id);
        return pane && requestFocus(*pane);
    }
    bool Root::capturePointer(Pane& pane) noexcept
    {
        requireOwner();
        if (pane.attachedRoot() != this || !pane.visible() || !impl_->context->windowFocused() || !allowedByModal(pane))
            return false;
        impl_->pointer_capture = &pane;
        return true;
    }
    void Root::releasePointer(Pane& pane) noexcept
    {
        requireOwner();
        if (impl_->pointer_capture.window == &pane)
            impl_->pointer_capture = {};
    }
    void Root::bindWindow(window::LuxWindow* window) noexcept
    {
        if (!impl_ && !window)
            return; // Detaching is valid after failed initialization.
        requireOwner();
        if (!impl_)
            return;
        impl_->window = window;
        impl_->context->bindWindow(window ? window->nativeHandle() : nullptr);
    }
    window::LuxWindow* Root::window() const noexcept
    {
        requireOwner();
        return impl_ ? impl_->window : nullptr;
    }

    lux::cxx::expected<void, ECaptureError> Root::update(FrameInfo info, DrawData* output) noexcept
    {
        requireOwner();
        const bool is_active_visit = impl_->drawing || impl_->updating || impl_->layout_depth != 0;
        const bool is_active_callback = impl_->change_batch_size != 0 || impl_->active_change || isDispatching();
        if (is_active_visit || is_active_callback)
            return lux::cxx::unexpected(ECaptureError::FRAME_OPEN);
        lux::cxx::expected<void, ECaptureError> result;
        if (output)
        {
            result = collectDrawData(info, *output);
            if (result)
            {
                // Freeze mutation until all references in the captured data have been pinned.
                impl_->updating = true;
                result = drawDataReady(*output);
                impl_->updating = false;
            }
        }
        // Input and accepted interactions still finish if capture/pinning failed.
        maintain();
        return result;
    }

    lux::cxx::expected<void, ECaptureError> Root::drawDataReady(const DrawData&) noexcept
    {
        return {};
    }

    void Root::deferChange(Pane& target, ChangeCallback apply) noexcept
    {
        requireOwner();
        if (target.attachedRoot() != this)
            detail::failContract();
        impl_->queueChange(target, apply);
    }

    void Root::deferChange(Element& target, ChangeCallback apply) noexcept
    {
        requireOwner();
        if (target.attachedRoot() != this)
            detail::failContract();
        impl_->queueChange(target, apply);
    }

    void Root::setMenu(std::vector<MenuItem> menu, std::shared_ptr<const void> source)
    {
        checkContentChange();
        const auto old_source = std::exchange(impl_->menu_source, std::move(source));
        impl_->menu = std::move(menu);
    }
    std::span<const MenuItem> Root::menu() const noexcept
    {
        requireOwner();
        return impl_->menu;
    }
    std::span<Pane* const> Root::panes() const noexcept
    {
        requireOwner();
        return impl_->windows;
    }
    std::uint64_t Root::windowRevision() const noexcept
    {
        requireOwner();
        return impl_->window_revision;
    }
    bool Root::menuTargets(const Element& element) const noexcept
    {
        requireOwner();
        return impl_->menu_open && impl_->menu_element == &element;
    }
    void Root::Impl::menuCommand(Root& root, Command& command, std::size_t index) noexcept
    {
        MenuRequest request{EMenuAction::COMMAND, menu_pane, menu_element, command, menu_source.get(), index};
        if (object::sendEvent(root, request))
        {
            command = request.command;
            return;
        }
        object::LuxObject* target = menu_element ? static_cast<object::LuxObject*>(menu_element) : menu_pane;
        if (!target)
            return;
        if (command.phase == ECommandPhase::QUERY)
            static_cast<void>(object::routeEvent(*target, root, command));
        else
        {
            menu_calls.push_back({target, CommandId{std::string(command.id.name())}});
            command.result = ECommandDispatchResult::EXECUTED;
        }
    }
    void Root::Impl::drawMenuItems(Root& root, std::span<const MenuItem> items) noexcept
    {
        for (const auto& item : items)
        {
            const auto* label = item.label.empty() ? "" : item.label.data();
            if (!item.children.empty())
            {
                if (ImGui::BeginMenu(label))
                {
                    drawMenuItems(root, item.children);
                    ImGui::EndMenu();
                }
                continue;
            }
            if (!item.command.isValid())
            {
                if (item.label.empty())
                    ImGui::Separator();
                else
                    ImGui::TextDisabled("%s", label);
                continue;
            }
            Command command{item.command};
            menuCommand(root, command, item.index);
            const auto* shortcut = item.shortcut_label.empty() ? "" : item.shortcut_label.data();
            if (ImGui::MenuItem(label, shortcut, command.checked, command.enabled))
            {
                command.phase = ECommandPhase::EXECUTE;
                menuCommand(root, command, item.index);
            }
        }
    }
    void Root::Impl::drawMenu(Root& root) noexcept
    {
        menu_height = 0;
        bool opened{};
        if (!menu.empty() && ImGui::BeginMainMenuBar())
        {
            menu_height = ImGui::GetWindowHeight();
            for (const auto& item : menu)
                if (ImGui::BeginMenu((item.label.empty() ? "" : item.label.data()), !modal))
                {
                    opened = true;
                    if (!menu_open)
                    {
                        menu_pane = focused;
                        menu_element = focused_element;
                        MenuRequest request{EMenuAction::OPEN, menu_pane, menu_element, {}, menu_source.get()};
                        static_cast<void>(object::sendEvent(root, request));
                        menu_open = true;
                    }
                    drawMenuItems(root, item.children);
                    ImGui::EndMenu();
                }
            ImGui::EndMainMenuBar();
        }
        if (menu_open && !opened)
        {
            MenuRequest request{EMenuAction::CLOSE, menu_pane, menu_element, {}, menu_source.get()};
            static_cast<void>(object::sendEvent(root, request));
            menu_pane = nullptr;
            menu_element = nullptr;
            menu_open = false;
        }
    }
    bool Root::Impl::shortcut(Root& root, const Key& key) noexcept
    {
        if (!key.down)
            return false;
        const bool control = (routed_modifiers & ImGuiMod_Ctrl) != 0;
        const bool shift = (routed_modifiers & ImGuiMod_Shift) != 0;
        const bool alt = (routed_modifiers & ImGuiMod_Alt) != 0;
        const auto find = [&](auto&& self, std::span<const MenuItem> items) -> const MenuItem*
        {
            for (const auto& item : items)
            {
                const auto& binding = item.shortcut;
                const bool matches = binding.key == key.key && binding.control == control && binding.shift == shift &&
                                     binding.alt == alt;
                if (item.command.isValid() && matches)
                    return &item;
                if (const auto* nested = self(self, item.children))
                    return nested;
            }
            return nullptr;
        };
        const auto* item = find(find, menu);
        if (!item)
            return false;
        menu_pane = focused;
        menu_element = focused_element;
        MenuRequest opened{EMenuAction::OPEN, menu_pane, menu_element, {}, menu_source.get()};
        static_cast<void>(object::sendEvent(root, opened));
        Command command{item->command};
        menuCommand(root, command, item->index);
        if (command.enabled)
        {
            command.phase = ECommandPhase::EXECUTE;
            menuCommand(root, command, item->index);
        }
        return true;
    }

    void Root::Impl::queueChange(object::LuxObject& target, ChangeCallback apply) noexcept
    {
        if (!apply)
            detail::failContract();
        const Change change{&target, apply};
        const auto pending = changes.begin() + change_batch_size;
        if (std::find(pending, changes.end(), change) == changes.end())
            changes.push_back(change);
    }

    void Root::Impl::cancelChanges(object::LuxObject& target) noexcept
    {
        for (auto& call : menu_calls)
            if (call.target == &target)
                call.target = nullptr;
        // The active batch keeps its indices even if a preceding callback destroys a later target.
        for (std::size_t index{}; index < change_batch_size; ++index)
            if (changes[index].target == &target)
                changes[index] = {};
        const auto pending = changes.begin() + change_batch_size;
        const auto end = std::remove_if(
            pending,
            changes.end(),
            [&target](const Change& change) noexcept { return change.target == &target; }
        );
        changes.erase(end, changes.end());
    }

    void Root::applyPendingChanges() noexcept
    {
        requireOwner();
        impl_->applyPendingChanges();
        const auto count = impl_->menu_calls.size();
        for (std::size_t index{}; index < count; ++index)
        {
            const auto call = impl_->menu_calls[index];
            if (!call.target)
                continue;
            Command command{call.command.view()};
            static_cast<void>(object::routeEvent(*call.target, *this, command));
            if (command.enabled)
            {
                command.phase = ECommandPhase::EXECUTE;
                static_cast<void>(object::routeEvent(*call.target, *this, command));
            }
        }
        impl_->menu_calls.erase(impl_->menu_calls.begin(), impl_->menu_calls.begin() + count);
    }

    bool Root::hasPendingChanges() const noexcept
    {
        requireOwner();
        return !impl_->changes.empty() || !impl_->menu_calls.empty();
    }

    void Root::Impl::applyPendingChanges() noexcept
    {
        const bool is_active_visit = drawing || updating || layout_depth != 0;
        const bool is_active_callback = change_batch_size != 0 || active_change || Root::isDispatching();
        if (is_active_visit || is_active_callback)
            detail::failContract();
        change_batch_size = changes.size();
        for (std::size_t index{}; index < change_batch_size; ++index)
        {
            // No vector reference survives the call: it may enqueue or destroy other targets.
            const auto change = std::exchange(changes[index], Change{});
            if (!change.target)
                continue;
            active_change = change.target;
            Root::beginCallbackBorrow(*change.target);
            change.apply(*change.target);
            Root::endCallbackBorrow(*change.target);
            active_change = nullptr;
        }
        changes.erase(changes.begin(), changes.begin() + change_batch_size);
        change_batch_size = 0;
    }

    lux::cxx::expected<void, ECaptureError> Root::collectDrawData(FrameInfo info, DrawData& output) noexcept
    {
        requireOwner();
        if (impl_->drawing || impl_->updating)
            return lux::cxx::unexpected(ECaptureError::FRAME_OPEN);
        detail::ContextActivation active{impl_->context->native()};
        impl_->drawing = true;
        struct Finish final
        {
            Root& root;
            ~Finish()
            {
                root.impl_->drawing = false;
            }
        } finish{*this};
        auto started = impl_->context->beginFrame(info);
        if (!started)
            return started;
        ++impl_->layout_epoch;
        impl_->draw_focused = impl_->draw_hovered = nullptr;
        impl_->draw_focused_element = impl_->draw_hovered_element = nullptr;
        impl_->drawMenu(*this);
        prepareLayout();
        for (auto* pane : impl_->windows)
            if (pane)
                drawPane(*pane);
        impl_->focused_element = impl_->draw_focused_element;
        impl_->hovered_element = impl_->draw_hovered_element;
        auto* old_focus = impl_->focused;
        auto* old_hover = impl_->hovered;
        if (old_focus != impl_->draw_focused)
        {
            impl_->focused = impl_->draw_focused;
            if (old_focus)
                old_focus->setFocused(false);
            if (impl_->draw_focused)
                impl_->draw_focused->setFocused(true);
        }
        if (old_hover)
            old_hover->setHovered(false);
        impl_->hovered = impl_->draw_hovered;
        if (impl_->draw_hovered)
            impl_->draw_hovered->setHovered(true);
        impl_->reset_docking = false;
        return impl_->context->endFrame(output);
    }

    PreparedAttachment::PreparedAttachment(std::unique_ptr<detail::AttachmentState> state) noexcept
        : state_(std::move(state))
    {
    }
    PreparedAttachment::~PreparedAttachment() noexcept
    {
        if (!state_)
            return;
        if (state_->root)
            state_->root->abandonAttachment(*state_);
        else
            for (auto* pane : state_->roots)
                if (pane && pane->preparation_ == state_.get())
                    pane->preparation_ = nullptr;
    }
    PreparedAttachment::PreparedAttachment(PreparedAttachment&&) noexcept = default;
    PreparedAttachment& PreparedAttachment::operator=(PreparedAttachment&& other) noexcept
    {
        PreparedAttachment previous(std::move(other));
        state_.swap(previous.state_);
        return *this;
    }
    void Root::abandonAttachment(detail::AttachmentState& state) noexcept
    {
        requireOwner();
        if (impl_->preparation == &state)
            impl_->preparation = nullptr;
        for (auto* pane : state.roots)
            if (pane && pane->preparation_ == &state)
                pane->preparation_ = nullptr;
        state.root = nullptr;
        state.valid = false;
    }
    bool Root::attachmentSafe() const noexcept
    {
        return isOnAffinityThread() && !impl_->drawing && !impl_->updating && !impl_->layout_depth &&
               !impl_->active_change && !impl_->committing_structure && !isDispatching();
    }

    cxx::expected<void, EAttachmentError> Root::addSubPane(Pane& pane) noexcept
    {
        auto attach = [&]() noexcept { return attachChild(pane); };
        return addSubPaneImpl(pane, attach);
    }

    cxx::expected<void, EAttachmentError> Root::addSubPaneImpl(
        Pane& pane, cxx::function_ref<object::ObjectResult<void>()> attach
    ) noexcept
    {
        return compose(*this, pane, false, attach);
    }

    cxx::expected<void, EAttachmentError> Root::removeSubPane(Pane& pane) noexcept
    {
        auto prepared = prepareDetach(pane);
        if (!prepared)
            return cxx::unexpected(prepared.error());
        auto removed = commit(*prepared);
        if (!removed)
            return cxx::unexpected(removed.error());
        return {};
    }

    cxx::expected<void, EAttachmentError> Root::compose(
        object::LuxObject& parent, object::LuxObject& child, bool replace,
        cxx::function_ref<object::ObjectResult<void>()> attach, Element* previous
    ) noexcept
    {
        const bool is_wrong_thread = !parent.isOnAffinityThread() || !child.isOnAffinityThread();
        if (is_wrong_thread)
            return cxx::unexpected(EAttachmentError::WRONG_THREAD);
        if (isDispatching())
            return cxx::unexpected(EAttachmentError::BUSY);
        auto* parent_root = dynamic_cast<Root*>(&parent);
        auto* parent_pane = dynamic_cast<Pane*>(&parent);
        auto* parent_element = dynamic_cast<Element*>(&parent);
        auto* child_pane = dynamic_cast<Pane*>(&child);
        auto* child_element = dynamic_cast<Element*>(&child);
        const bool is_content = parent_pane && child_element;
        const bool is_valid_topology = ((parent_root || parent_pane) && child_pane) ||
            is_content || (parent_element && child_element);
        if (!is_valid_topology)
            return cxx::unexpected(EAttachmentError::INVALID_TREE);
        auto* root = parent_root ? parent_root : parent_pane ? parent_pane->root_ : parent_element->attachedRoot();
        const bool is_parent_closed = parent_root ? parent_root->isClosing() :
            parent_pane ? parent_pane->isClosing() : parent_element->isClosing();
        const bool is_child_closed = child_pane ? child_pane->isClosing() : child_element->isClosing();
        if (is_parent_closed || is_child_closed)
            return cxx::unexpected(EAttachmentError::CLOSED);
        if (root)
        {
            const bool is_busy = root->impl_->drawing || root->impl_->layout_depth ||
                root->impl_->committing_structure || root->impl_->preparation;
            if (is_busy)
                return cxx::unexpected(EAttachmentError::BUSY);
        }
        if (is_content)
            previous = parent_pane->content_;
        const bool is_invalid_previous = previous && previous->parent() != &parent;
        if (is_invalid_previous)
            return cxx::unexpected(EAttachmentError::NOT_ATTACHED);
        if (root && previous)
            for (auto* callback : {root->impl_->active_update, root->impl_->active_change})
                for (auto* active = callback; active; active = active->parent())
                    if (active == previous)
                        return cxx::unexpected(EAttachmentError::BUSY);
        if (previous && (!replace || previous == &child))
            return cxx::unexpected(EAttachmentError::OCCUPIED);
        const bool is_existing_content_child = is_content && child.parent() == &parent;
        if (child.parent() && !is_existing_content_child)
            return cxx::unexpected(EAttachmentError::ALREADY_ATTACHED);
        for (auto* ancestor = &parent; ancestor; ancestor = ancestor->parent())
            if (ancestor == &child)
                return cxx::unexpected(EAttachmentError::INVALID_TREE);
        if (previous && previous->ownership() == object::EObjectOwnership::PARENT_OWNED &&
            !previous->isOnAffinityThread())
            return cxx::unexpected(EAttachmentError::WRONG_DISPATCHER);

        detail::AttachmentState prepared;
        bool invalid{};
        visitSubtree(child, [&](object::LuxObject& node) noexcept {
            if (auto* pane = dynamic_cast<Pane*>(&node))
            {
                invalid |= pane->attachedRoot() != nullptr;
                prepared.nodes.push_back({pane, nullptr});
            }
            else if (auto* element = dynamic_cast<Element*>(&node))
            {
                invalid |= element->attachedRoot() && element->attachedRoot() != root;
                if (element->registration_slot_ == SIZE_MAX)
                    prepared.nodes.push_back({nullptr, element});
            }
            else
                invalid = true;
        });
        if (invalid)
            return cxx::unexpected(EAttachmentError::INVALID_TREE);
        if (child_element)
        {
            for (auto* sibling = parent.firstChild(); sibling; sibling = sibling->nextSibling())
            {
                auto* element = dynamic_cast<Element*>(sibling);
                const bool is_conflicting_id = element && !element->isClosing() &&
                    element != &child && element != previous &&
                    element->id().view() == child_element->id().view();
                if (is_conflicting_id)
                    return cxx::unexpected(EAttachmentError::DUPLICATE_ID);
            }
        }
        if (root)
        {
            std::size_t removing{};
            if (previous)
                visitSubtree(*previous, [&](object::LuxObject&) noexcept { ++removing; });
            auto reserved = root->prepareRegistration(prepared, removing);
            if (!reserved)
                return reserved;
            // A stateful deleter can run foreign cleanup while it moves. Reentrant UI structure
            // operations must fail before this still-owned candidate or any registration changes.
            root->impl_->committing_structure = true;
        }
        beginCallbackBorrow(parent);
        auto adopted = attach();
        if (root)
            root->impl_->committing_structure = false;
        if (!adopted)
        {
            endCallbackBorrow(parent);
            using enum object::EObjectTreeError;
            switch (adopted.error())
            {
            case WRONG_THREAD: return cxx::unexpected(EAttachmentError::WRONG_THREAD);
            case BUSY: return cxx::unexpected(EAttachmentError::BUSY);
            case CLOSED: return cxx::unexpected(EAttachmentError::CLOSED);
            case ALREADY_ATTACHED: return cxx::unexpected(EAttachmentError::ALREADY_ATTACHED);
            default: return cxx::unexpected(EAttachmentError::INVALID_TREE);
            }
        }
        // From here to notifications, all storage is reserved and no provider is invoked.
        if (previous)
        {
            if (root)
                root->releaseSubtree(*previous, false);
            if (previous->ownership() == object::EObjectOwnership::PARENT_OWNED)
            {
                if (!previous->requestDestruction())
                    detail::failContract();
                previous->beginDestruction();
            }
            else
                previous->detachFromParent();
            previous->assignPane(nullptr);
            previous->element_parent_ = nullptr;
        }
        if (child_element)
        {
            child_element->element_parent_ = parent_element;
            child_element->assignPane(parent_pane ? parent_pane : parent_element->containingPane());
            if (is_content)
                parent_pane->content_ = child_element;
        }
        if (parent_pane)
            parent_pane->invalidatePreparation();
        else if (parent_element && parent_element->pane_)
            parent_element->pane_->invalidatePreparation();
        if (root)
        {
            for (const auto node : prepared.nodes)
            {
                if (node.pane)
                {
                    node.pane->root_ = root;
                    root->registerPane(*node.pane);
                }
                else
                    root->registerElement(*node.element);
            }
            if (previous)
                root->releaseSubtree(*previous, true);
            if (child_pane)
            {
                child_pane->beginTreeVisit();
                static_cast<void>(root->emit(root->attachmentChanged, AttachmentChanged{child_pane->id(), true}));
                child_pane->endTreeVisit();
            }
        }
        endCallbackBorrow(parent);
        return {};
    }

    cxx::expected<void, EAttachmentError> Root::prepareRegistration(
        detail::AttachmentState& prepared, std::size_t removing
    )
    {
        std::set<std::string_view> names;
        for (const auto node : prepared.nodes)
            if (node.pane)
            {
                if (node.pane->attachment_epoch_ == UINT64_MAX)
                {
                    return cxx::unexpected(EAttachmentError::CAPACITY);
                }
                const auto name = node.pane->id().name();
                if (findPane(node.pane->id().view()) || !names.emplace(name).second)
                    return cxx::unexpected(EAttachmentError::DUPLICATE_ID);
            }
        const auto active = static_cast<std::size_t>(
            std::ranges::count_if(impl_->registrations, [](const auto& entry) { return entry.object; })
        );
        const auto remaining = active - std::min(active, removing);
        const auto available = impl_->attachment_capacity - std::min(remaining, impl_->attachment_capacity);
        if (prepared.nodes.size() > available)
            return cxx::unexpected(EAttachmentError::CAPACITY);
        if (!impl_->updating)
            impl_->compactRegistrations();
        impl_->registrations.reserve(impl_->registrations.size() + prepared.nodes.size());
        impl_->windows.reserve(impl_->windows.size() + names.size());
        return {};
    }
    cxx::expected<AttachmentCommit, EAttachmentError> Root::addSubPanes(
        std::span<std::unique_ptr<Pane, object::ObjectDeleter>> owners,
        std::span<const WindowVisibility> visibility,
        PreparedDockTree* docking
    ) noexcept
    {
        std::vector<Pane*> panes;
        panes.reserve(owners.size());
        for (const auto& owner : owners)
        {
            panes.push_back(owner.get());
        }
        auto prepared = prepareMount(panes, visibility);
        if (!prepared)
            return cxx::unexpected(prepared.error());
        auto apply_docking = [&]() noexcept { if (docking) commitDockTree(std::move(*docking)); };
        return commit(*prepared, owners, apply_docking);
    }
    cxx::expected<AttachmentCommit, EAttachmentError> Root::commit(
        PreparedAttachment& prepared,
        std::span<std::unique_ptr<Pane, object::ObjectDeleter>> owners,
        cxx::function_ref<void()> apply
    ) noexcept
    {
        if (auto valid = validateAttachment(prepared); !valid)
            return cxx::unexpected(valid.error());
        const auto& state = *prepared.state_;
        const bool is_invalid_batch = !state.mount || owners.size() != state.roots.size();
        if (is_invalid_batch)
            return cxx::unexpected(EAttachmentError::INVALID_TREE);
        std::vector<object::LuxObject*> objects;
        objects.reserve(owners.size());
        for (std::size_t i{}; i < owners.size(); ++i)
        {
            if (owners[i].get() != state.roots[i])
                return cxx::unexpected(EAttachmentError::INVALID_TREE);
            objects.push_back(owners[i].get());
        }
        impl_->committing_structure = true;
        auto transfer = [&](std::size_t index) noexcept
        {
            auto deleter = std::move(owners[index].get_deleter());
            static_cast<void>(owners[index].release());
            return deleter;
        };
        auto adopted = adoptChildren(objects, transfer);
        impl_->committing_structure = false;
        if (!adopted)
        {
            using enum object::EObjectTreeError;
            switch (adopted.error())
            {
            case WRONG_THREAD: return cxx::unexpected(EAttachmentError::WRONG_THREAD);
            case BUSY: return cxx::unexpected(EAttachmentError::BUSY);
            case CLOSED: return cxx::unexpected(EAttachmentError::CLOSED);
            case ALREADY_ATTACHED: return cxx::unexpected(EAttachmentError::ALREADY_ATTACHED);
            default: return cxx::unexpected(EAttachmentError::INVALID_TREE);
            }
        }
        // The transfer uses host ObjectDeleter moves, not extension callbacks. The validated token
        // and prepared routing remain valid throughout this no-callback ownership commit.
        prepared.state_->adopted = true;
        return commitPrepared(prepared, apply);
    }

    Root::AttachmentResult Root::prepareMount(Pane& pane)
    {
        auto* value = &pane;
        return prepareMount(std::span<Pane* const>{&value, 1});
    }
    Root::AttachmentResult Root::prepareDetach(Pane& pane)
    {
        auto* value = &pane;
        return prepareDetach(std::span<Pane* const>{&value, 1});
    }
    Root::AttachmentResult Root::prepareMount(
        std::span<Pane* const> panes,
        std::span<const WindowVisibility> visibility
    )
    {
        return prepareAttachment(panes, true, visibility);
    }
    Root::AttachmentResult Root::prepareDetach(std::span<Pane* const> panes)
    {
        return prepareAttachment(panes, false);
    }
    Root::AttachmentResult Root::prepareAttachment(
        std::span<Pane* const> panes,
        bool mount,
        std::span<const WindowVisibility> visibility
    )
    {
        if (!isOnAffinityThread())
            return lux::cxx::unexpected(EAttachmentError::WRONG_THREAD);
        if (isClosing())
            return lux::cxx::unexpected(EAttachmentError::CLOSED);
        if (!attachmentSafe() || impl_->preparation)
            return lux::cxx::unexpected(EAttachmentError::BUSY);
        auto prepared = std::make_unique<detail::AttachmentState>();
        prepared->roots.reserve(panes.size());
        for (auto* pane : panes)
        {
            if (!pane || std::ranges::find(prepared->roots, pane) != prepared->roots.end())
                return lux::cxx::unexpected(EAttachmentError::INVALID_TREE);
            if (!pane->isOnAffinityThread())
                return lux::cxx::unexpected(EAttachmentError::WRONG_THREAD);
            if (pane->isClosing())
                return lux::cxx::unexpected(EAttachmentError::CLOSED);
            if (pane->preparation_)
                return lux::cxx::unexpected(EAttachmentError::BUSY);
            if (mount && (pane->attachedRoot() || pane->parent()))
                return lux::cxx::unexpected(EAttachmentError::ALREADY_ATTACHED);
            if (!mount && pane->attachedRoot() != this)
                return lux::cxx::unexpected(EAttachmentError::NOT_ATTACHED);
            const bool is_unretirable = !mount &&
                pane->ownership() == object::EObjectOwnership::PARENT_OWNED && !pane->isOnAffinityThread();
            if (is_unretirable)
                return lux::cxx::unexpected(EAttachmentError::WRONG_DISPATCHER);
            prepared->roots.push_back(pane);
        }
        // A batch cannot include both a subtree and one of its descendants.
        for (auto* pane : prepared->roots)
            for (auto* ancestor = pane->parent(); ancestor; ancestor = ancestor->parent())
                if (std::ranges::find(prepared->roots, ancestor) != prepared->roots.end())
                    return cxx::unexpected(EAttachmentError::INVALID_TREE);
        bool invalid{};
        const auto visit = [&](object::LuxObject& node) noexcept
        {
            if (auto* window = dynamic_cast<Pane*>(&node))
            {
                if (!mount && window->registration_slot_ == SIZE_MAX)
                    return; // Already removed; Object still owns its pending mechanical reclamation.
                const bool wrong_root = mount ? window->attachedRoot() != nullptr : window->attachedRoot() != this;
                invalid |= wrong_root;
                prepared->nodes.push_back({window, nullptr});
            }
            else if (auto* element = dynamic_cast<Element*>(&node))
            {
                if (!mount && element->registration_slot_ == SIZE_MAX)
                    return;
                invalid |= !element->containingPane();
                prepared->nodes.push_back({nullptr, element});
            }
            else
            {
                invalid = true;
                return;
            }
        };
        for (auto* pane : panes)
            visitSubtree(*pane, visit);
        if (invalid)
            return lux::cxx::unexpected(EAttachmentError::INVALID_TREE);
        prepared->visibility.reserve(visibility.size());
        prepared->visibility_changed.reserve(visibility.size());
        for (const auto& value : visibility)
        {
            if (!value.pane)
                return lux::cxx::unexpected(EAttachmentError::INVALID_TREE);
            const bool is_existing = value.pane->attachedRoot() == this;
            const bool is_candidate =
                std::ranges::any_of(prepared->nodes, [&](const auto& node) { return node.pane == value.pane; });
            const bool is_duplicate =
                std::ranges::any_of(prepared->visibility, [&](const auto& prior) { return prior.pane == value.pane; });
            if ((!is_existing && !is_candidate) || is_duplicate)
                return lux::cxx::unexpected(EAttachmentError::INVALID_TREE);
            prepared->visibility.push_back(value);
        }
        if (mount)
        {
            auto reserved = prepareRegistration(*prepared);
            if (!reserved)
                return cxx::unexpected(reserved.error());
        }
        prepared->root = this;
        prepared->revision = impl_->structure_revision;
        prepared->window_revision = impl_->window_revision;
        prepared->mount = mount;
        impl_->preparation = prepared.get();
        for (auto* pane : panes)
            pane->preparation_ = prepared.get();
        return PreparedAttachment{std::move(prepared)};
    }
    lux::cxx::expected<AttachmentCommit, EAttachmentError> Root::commit(PreparedAttachment& token) noexcept
    {
        const auto adopt = []() noexcept {};
        return commit(token, adopt);
    }
    lux::cxx::expected<AttachmentCommit, EAttachmentError> Root::commit(
        PreparedAttachment& token,
        cxx::function_ref<void()> adopt
    ) noexcept
    {
        if (auto valid = validateAttachment(token); !valid)
            return cxx::unexpected(valid.error());
        return commitPrepared(token, adopt);
    }
    cxx::expected<void, EAttachmentError> Root::validateAttachment(const PreparedAttachment& token) const noexcept
    {
        if (!isOnAffinityThread())
            return lux::cxx::unexpected(EAttachmentError::WRONG_THREAD);
        if (!attachmentSafe())
            return lux::cxx::unexpected(EAttachmentError::BUSY);
        auto* state = token.state_.get();
        const bool stale = !state || state->root != this || !state->valid ||
                           state->revision != impl_->structure_revision ||
                           state->window_revision != impl_->window_revision;
        if (stale)
            return lux::cxx::unexpected(EAttachmentError::STALE_PREPARATION);
        return {};
    }
    AttachmentCommit Root::commitPrepared(PreparedAttachment& token, cxx::function_ref<void()> adopt) noexcept
    {
        auto* state = token.state_.get();
        // Consume the public token before notifications. Callers may release or replace it in a callback.
        auto committed = std::move(token.state_);
        const bool mount = state->mount;
        // Clear preparation before changing links. No callbacks or allocations until every link is adopted.
        abandonAttachment(*state);
        if (mount)
        {
            if (!state->adopted)
                for (auto* pane : state->roots)
                    pane->attachTo(*this);
            for (auto node : state->nodes)
            {
                if (node.pane)
                {
                    node.pane->root_ = this;
                    registerPane(*node.pane);
                }
                else
                    registerElement(*node.element);
            }
        }
        else
        {
            for (auto node : state->nodes)
                if (node.pane)
                    unregisterPane(*node.pane, false);
                else
                    unregisterElement(*node.element, false);
            for (auto* pane : state->roots)
            {
                if (pane->ownership() == object::EObjectOwnership::PARENT_OWNED)
                {
                    if (!pane->requestDestruction())
                        detail::failContract();
                    pane->beginDestruction();
                }
                else
                    pane->detachFromParent();
            }
            for (auto node : state->nodes)
                if (node.pane)
                    node.pane->root_ = nullptr;
        }
        for (auto value : state->visibility)
            if (value.pane->visible_ != value.visible)
            {
                value.pane->visible_ = value.visible;
                state->visibility_changed.push_back(value.pane);
                ++impl_->window_revision;
            }
        adopt();
        AttachmentCommit result{mount, {}};
        const auto append = [&](object::SignalDelivery delivered)
        {
            result.notifications.direct += delivered.direct;
            result.notifications.queued += delivered.queued;
            result.notifications.full += delivered.full;
            result.notifications.closed += delivered.closed;
        };
        // A notification cannot mutate/destroy this subtree, even after it has left Root's routing chain.
        for (auto* pane : state->roots)
            pane->beginTreeVisit();
        if (!mount)
            for (auto node : state->nodes)
                append(emit(objectRemoved, node.pane ? static_cast<object::LuxObject*>(node.pane) : node.element));
        for (auto* pane : state->roots)
            append(emit(attachmentChanged, AttachmentChanged{pane->id(), mount}));
        for (auto* pane : state->visibility_changed)
            append(pane->emit(pane->visibilityChanged, PaneVisibilityChanged{pane->visible_}));
        for (auto* pane : state->roots)
            pane->endTreeVisit();
        return result;
    }

    void Root::releaseSubtree(object::LuxObject& subtree, bool notify) noexcept
    {
        // All routing is revoked before observers see removal. The actual owner remains Object.
        visitSubtree(subtree, [&](object::LuxObject& node) noexcept {
            if (auto* pane = dynamic_cast<Pane*>(&node))
            {
                if (pane->registration_slot_ != SIZE_MAX)
                    unregisterPane(*pane, false);
                pane->root_ = nullptr;
            }
            else if (auto* element = dynamic_cast<Element*>(&node))
            {
                if (element->registration_slot_ != SIZE_MAX)
                    unregisterElement(*element, false);
            }
        });
        if (notify)
        {
            auto* pane = dynamic_cast<Pane*>(&subtree);
            auto* element = dynamic_cast<Element*>(&subtree);
            if (pane)
                pane->beginTreeVisit();
            else if (element)
                element->beginTreeVisit();
            else
                beginTreeVisit();
            visitSubtree(subtree, [&](object::LuxObject& node) noexcept {
                static_cast<void>(emit(objectRemoved, &node));
            });
            if (pane)
                pane->endTreeVisit();
            else if (element)
                element->endTreeVisit();
            else
                endTreeVisit();
        }
    }

    void Root::prepareChildrenRelease(object::LuxObject& owner) noexcept
    {
        for (auto* child = owner.firstChild(); child; child = child->nextSibling())
        {
            auto* pane = dynamic_cast<Pane*>(child);
            auto* element = dynamic_cast<Element*>(child);
            auto* root = pane ? pane->attachedRoot() : element ? element->attachedRoot() : nullptr;
            if (root)
                root->releaseSubtree(*child, true);
        }
        // Only derived UI associations are cleared here. Object performs every unlink and deletion.
        // External Pane subtrees survive intact; elements losing their containing Pane do not.
        auto* node = owner.firstChild();
        while (node)
        {
            if (auto* element = dynamic_cast<Element*>(node))
            {
                if (element->pane_)
                    element->assignPane(nullptr);
                element->element_parent_ = nullptr;
            }
            const bool descend = node->ownership() == object::EObjectOwnership::PARENT_OWNED && node->firstChild();
            if (descend)
                node = node->firstChild();
            else
            {
                while (node->parent() != &owner && !node->nextSibling())
                    node = node->parent();
                node = node->nextSibling();
            }
        }
    }

    void Root::registerPane(Pane& pane)
    {
        if (pane.attachment_epoch_ == UINT64_MAX)
        {
            detail::failContract(); // Legacy rooted constructors cannot report a failed attachment.
        }
        ++pane.attachment_epoch_;
        ++impl_->structure_revision;
        ++impl_->window_revision;
        checkContentChange();
        // Reserve all fallible storage before publishing either borrowed registration.
        const auto count = impl_->registrations.size() + 1;
        if (count > impl_->registrations.capacity())
            impl_->registrations.reserve(std::max(count, count * 2));
        if (impl_->windows.size() == impl_->windows.capacity())
            impl_->windows.reserve(std::max<std::size_t>(8, impl_->windows.size() * 2));
        pane.registration_slot_ = impl_->registrations.size();
        pane.window_slot_ = impl_->windows.size();
        impl_->registrations.push_back(
            {&pane,
             [](object::LuxObject* value) noexcept { static_cast<Pane*>(value)->update(); },
             &pane.registration_slot_}
        );
        impl_->windows.push_back(&pane);
    }

    void Root::registerElement(Element& element)
    {
        ++impl_->structure_revision;
        checkContentChange();
        const auto count = impl_->registrations.size() + 1;
        if (count > impl_->registrations.capacity())
            impl_->registrations.reserve(std::max(count, count * 2));
        element.registration_slot_ = impl_->registrations.size();
        impl_->registrations.push_back(
            {&element,
             [](object::LuxObject* value) noexcept { static_cast<Element*>(value)->update(); },
             &element.registration_slot_}
        );
    }

    void Root::paneLabelChanged() noexcept
    {
        requireOwner();
        ++impl_->window_revision;
    }

    void Root::unregisterPane(Pane& pane, bool notify) noexcept
    {
        checkDestruction(pane);
        checkContentChange();
        ++impl_->structure_revision;
        if (impl_->menu_pane == &pane)
        {
            impl_->menu_pane = nullptr;
            impl_->menu_element = nullptr;
        }
        ++impl_->window_revision;
        impl_->cancelChanges(pane);
        impl_->registrations[pane.registration_slot_] = {};
        impl_->registration_holes = impl_->window_holes = true;
        impl_->windows[pane.window_slot_] = nullptr;
        for (auto** target :
             {&impl_->focused,
              &impl_->hovered,
              &impl_->pending_focus,
              &impl_->draw_focused,
              &impl_->draw_hovered,
              &impl_->modal})
            if (*target == &pane)
                *target = nullptr;
        if (impl_->pointer_capture.window == &pane)
            impl_->pointer_capture = {};
        pane.registration_slot_ = pane.window_slot_ = SIZE_MAX;
        pane.focused_ = pane.hovered_ = false;
        pane.close_requested_ = false;
        if (notify)
            static_cast<void>(emit(objectRemoved, static_cast<object::LuxObject*>(&pane)));
    }

    void Root::unregisterElement(Element& element, bool notify) noexcept
    {
        checkDestruction(element);
        checkContentChange();
        ++impl_->structure_revision;
        if (impl_->menu_element == &element)
            impl_->menu_element = nullptr;
        impl_->cancelChanges(element);
        impl_->registrations[element.registration_slot_] = {};
        impl_->registration_holes = true;
        if (impl_->focused_element == &element)
        {
            detail::ContextActivation context{impl_->context->native()};
            ImGui::ClearActiveID();
        }
        for (auto** target :
             {&impl_->focused_element,
              &impl_->hovered_element,
              &impl_->pending_element,
              &impl_->draw_focused_element,
              &impl_->draw_hovered_element})
            if (*target == &element)
                *target = nullptr;
        if (impl_->pointer_capture.element == &element)
            impl_->pointer_capture = {};
        element.registration_slot_ = SIZE_MAX;
        element.hovered_ = false;
        if (notify)
            static_cast<void>(emit(objectRemoved, static_cast<object::LuxObject*>(&element)));
    }

    void Root::checkDestruction(const object::LuxObject& object) const noexcept
    {
        requireOwner();
        for (auto* callback : {impl_->active_update, impl_->active_change})
            for (auto* active = callback; active; active = active->parent())
                if (active == &object)
                    detail::failContract();
    }

    void Root::checkContentChange() const noexcept
    {
        requireOwner();
        const bool is_frozen_visit = impl_->drawing || impl_->layout_depth != 0 || isDispatching() ||
            impl_->committing_structure;
        const bool is_frozen_update = impl_->updating && !impl_->active_update;
        if (is_frozen_visit || is_frozen_update)
            detail::failContract();
    }

    void Root::maintain() noexcept
    {
        requireOwner();
        if (impl_->drawing || impl_->updating)
            detail::failContract();
        impl_->updating = true;
        if (impl_->context->takeFocusLoss())
        {
            // Loss ends the current interaction even while presentation has no writable frame.
            // Its older native batch is still consumed by ImGui, but cannot replay business commands.
            deliverWindowFocus(false);
        }
        // Losing capture is observable even when no new native input or UI frame
        // arrives (for example, the owner hid its content during maintenance).
        if (const auto capture = impl_->pointer_capture;
            capture && (!capture.visible() || !allowedByModal(*capture.pane())))
        {
            impl_->pointer_capture = {};
            VInputEvent cancelled = PointerCancel{};
            static_cast<void>(object::sendEvent(*capture, cancelled));
        }
        if (impl_->context->hasInput())
        {
            detail::ContextActivation context{impl_->context->native()};
            routeInput();
        }
        // Construction order is parent-before-child. New registrations belong to the next turn;
        // removals leave tombstones so a callback may replace its children without invalidating this batch.
        const auto count = impl_->registrations.size();
        for (std::size_t index{}; index < count; ++index)
        {
            const auto entry = impl_->registrations[index];
            if (!entry.object)
                continue;
            impl_->active_update = entry.object;
            beginCallbackBorrow(*entry.object);
            entry.update(entry.object);
            endCallbackBorrow(*entry.object);
            impl_->active_update = nullptr;
        }
        impl_->compactRegistrations();
        impl_->updating = false;
    }

    Element* Root::focusedElement() const noexcept
    {
        requireOwner();
        return impl_->focused_element;
    }
    bool Root::requestFocus(Element& element) noexcept
    {
        if (element.attachedRoot() != this || !element.displayed() || !element.enabled() || !allowedByModal(element))
            return false;
        impl_->pending_element = &element;
        return requestFocus(element.pane());
    }
    bool Root::capturePointer(Element& element) noexcept
    {
        requireOwner();
        if (element.attachedRoot() != this || !element.displayed() || !element.enabled() || !impl_->context->windowFocused() ||
            !allowedByModal(element))
            return false;
        impl_->pointer_capture = &element;
        return true;
    }
    void Root::releaseFocus(Element& element) noexcept
    {
        requireOwner();
        if (element.attachedRoot() != this)
            detail::failContract();
        if (impl_->pending_element == &element)
            impl_->pending_element = {};
        if (impl_->focused_element != &element)
            return;
        detail::ContextActivation context{impl_->context->native()};
        ImGui::ClearActiveID();
        impl_->focused_element = {};
        releasePointer(element);
    }
    void Root::releasePointer(Element& element) noexcept
    {
        requireOwner();
        if (impl_->pointer_capture.element == &element)
            impl_->pointer_capture = {};
    }
    SizeHint Root::measureElement(Element& element, float width, bool intrinsic) noexcept
    {
        requireOwner();
        detail::ContextActivation context{impl_->context->native()};
        if (impl_->layout_depth++ == 0 && !impl_->drawing)
            ++impl_->layout_epoch;
        const auto epoch = impl_->layout_epoch;
        if (intrinsic)
        {
            if (element.hint_epoch_ != epoch)
            {
                element.intrinsic_hint_ = element.constrain(element.sizeHintContent());
                element.hint_epoch_ = epoch;
            }
        }
        else if (element.measure_epoch_ != epoch || element.measured_width_ != width)
        {
            element.measured_hint_ = element.constrain(element.measureContent(width));
            element.measure_epoch_ = epoch;
            element.measured_width_ = width;
        }
        --impl_->layout_depth;
        return intrinsic ? element.intrinsic_hint_ : element.measured_hint_;
    }
    void Root::arrangeElement(Element& element) noexcept
    {
        requireOwner();
        detail::ContextActivation context{impl_->context->native()};
        if (impl_->layout_depth++ == 0 && !impl_->drawing)
            ++impl_->layout_epoch;
        static_cast<void>(element.measure(element.rect().size.width));
        element.arrangeContent();
        --impl_->layout_depth;
    }
    void Root::drawElement(Element& element, Point parent_origin) noexcept
    {
        if (!impl_->drawing || element.attachedRoot() != this)
            detail::failContract();
        if (!element.visible_)
            return;
        auto& rect = element.rect_;
        element.draw_origin_ = {parent_origin.x + rect.position.x, parent_origin.y + rect.position.y};
        const auto origin = element.draw_origin_;
        ImGui::PushID(element.id().name().data(), element.id().name().data() + element.id().name().size());
        ImGui::SetCursorScreenPos({origin.x, origin.y});
        ImGui::PushClipRect({origin.x, origin.y}, {origin.x + rect.size.width, origin.y + rect.size.height}, true);
        ImGui::BeginDisabled(!element.enabled());
        ImGui::BeginGroup();
        auto* prior_focus = impl_->draw_focused_element;
        auto* prior_hover = impl_->draw_hovered_element;
        const bool requested = impl_->pending_element == &element;
        if (requested)
            ImGui::SetKeyboardFocusHere();
        element.draw();
        ImGui::EndGroup();
        element.hovered_ = ImGui::IsItemHovered();
        const bool clicked =
            element.hovered_ && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::IsMouseClicked(2));
        const bool retained = focusedElement() == &element && ImGui::IsWindowFocused();
        if (impl_->draw_hovered_element == prior_hover && element.hovered_)
            impl_->draw_hovered_element = &element;
        if (impl_->draw_focused_element == prior_focus && element.enabled() &&
            (requested || clicked || retained || ImGui::IsItemFocused()))
            impl_->draw_focused_element = &element;
        if (requested)
            impl_->pending_element = {};
        ImGui::EndDisabled();
        ImGui::PopClipRect();
        ImGui::PopID();
    }

    void Root::prepareLayout() noexcept
    {
        impl_->placements = {};
        if (impl_->docking)
        {
            const auto* viewport = ImGui::GetMainViewport();
            const ImGuiID root = ImGui::GetID("lux.ui.dockspace");
            if (impl_->pending_dock_tree)
            {
                auto prepared = std::move(impl_->pending_dock_tree);
                const auto& tree = prepared->tree;
                // Unlisted windows keep their owners and position. Removing the replaced DockSpace
                // only undocks them; the new tree never infers additional content or view creation.
                for (const auto& surface : tree.surfaces)
                {
                    auto flags = surface.floating ? ImGuiDockNodeFlags_None : ImGuiDockNodeFlags_DockSpace;
                    if (!surface.floating)
                        ImGui::DockBuilderRemoveNode(root);
                    const auto id = ImGui::DockBuilderAddNode(surface.floating ? 0 : root, flags);
                    prepared->ids[surface.node] = id;
                    const auto position = surface.floating
                                              ? ImVec2{surface.bounds.position.x, surface.bounds.position.y}
                                              : viewport->WorkPos;
                    const auto size = surface.floating ? ImVec2{surface.bounds.size.width, surface.bounds.size.height}
                                                       : viewport->WorkSize;
                    ImGui::DockBuilderSetNodePos(id, position);
                    ImGui::DockBuilderSetNodeSize(id, size);
                }
                for (auto index : prepared->order)
                {
                    const auto& node = tree.nodes[index];
                    const auto id = prepared->ids[index];
                    if (node.split != EDockSplit::LEAF)
                        ImGui::DockBuilderSplitNode(
                            id,
                            node.split == EDockSplit::HORIZONTAL ? ImGuiDir_Left : ImGuiDir_Up,
                            node.ratio,
                            &prepared->ids[node.first],
                            &prepared->ids[node.second]
                        );
                    else
                        for (const auto& window : node.windows)
                        {
                            const auto label = std::string{"###"} + window;
                            ImGui::DockBuilderDockWindow(label.c_str(), id);
                        }
                }
                for (const auto& surface : tree.surfaces)
                    ImGui::DockBuilderFinish(prepared->ids[surface.node]);
                impl_->split_layout.reset();
                impl_->dock_layout_initialized = true;
            }
            if (impl_->split_layout && !impl_->dock_layout_initialized && viewport->WorkSize.x > 0 &&
                viewport->WorkSize.y > 0)
            {
                impl_->dock_layout_initialized = true;
                const auto& layout = *impl_->split_layout;
                ImGui::DockBuilderRemoveNode(root);
                ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(root, viewport->WorkSize);
                ImGuiID center = root;
                auto& regions = impl_->dock_regions;
                if (!layout.toolbar.empty())
                {
                    ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.12F, &regions[4], &center);
                }
                if (!layout.bottom.empty())
                {
                    ImGui::DockBuilderSplitNode(
                        center,
                        ImGuiDir_Down,
                        std::clamp(layout.bottom_height / viewport->WorkSize.y, 0.1F, 0.4F),
                        &regions[3],
                        &center
                    );
                }
                if (!layout.left.empty())
                {
                    ImGui::DockBuilderSplitNode(
                        center,
                        ImGuiDir_Left,
                        std::clamp(layout.left_width / viewport->WorkSize.x, 0.1F, 0.3F),
                        &regions[0],
                        &center
                    );
                }
                if (!layout.right.empty())
                {
                    ImGui::DockBuilderSplitNode(
                        center,
                        ImGuiDir_Right,
                        std::clamp(
                            layout.right_width / std::max(1.0F, viewport->WorkSize.x - layout.left_width),
                            0.1F,
                            0.4F
                        ),
                        &regions[2],
                        &center
                    );
                }
                regions[1] = center;
                if (impl_->reset_docking)
                    for (auto* pane : impl_->windows)
                        if (pane && !pane->modal_)
                            ImGui::DockBuilderDockWindow(pane->window_label_.c_str(), center);
                ImGui::DockBuilderFinish(root);
            }
            // Submit even when panes are hidden: docking owns their persistent placement.
            ImGui::DockSpaceOverViewport(root, viewport);
        }
        if (impl_->split_layout && !impl_->docking)
        {
            auto& layout = *impl_->split_layout;
            const auto size = ImGui::GetIO().DisplaySize;
            const float top = impl_->menu_height + (layout.toolbar.empty() ? 0 : 38.0F);
            const auto visible = [&](const std::string& id)
            {
                const auto* pane = findPane(PaneIdView{id});
                return pane && pane->visible();
            };
            const float left =
                size.x >= 700 && visible(layout.left) ? std::clamp(layout.left_width, 160.0F, size.x * 0.3F) : 0;
            const float right =
                size.x >= 1000 && visible(layout.right) ? std::clamp(layout.right_width, 220.0F, size.x * 0.35F) : 0;
            const float bottom =
                size.y >= 450 && visible(layout.bottom) ? std::clamp(layout.bottom_height, 100.0F, size.y * 0.4F) : 0;
            const auto splitter = [&](const char* id, ImVec2 pos, ImVec2 extent, bool vertical, float& value)
            {
                ImGui::SetNextWindowPos(pos);
                ImGui::SetNextWindowSize(extent);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 0});
                ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2{1, 1});
                ImGui::Begin(
                    id,
                    nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoNav
                );
                ImGui::InvisibleButton("##split", extent);
                if (ImGui::IsItemActive())
                {
                    value += vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
                }
                if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                {
                    ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
                }
                ImGui::End();
                ImGui::PopStyleVar(2);
            };
            if (left > 0)
            {
                layout.left_width = left;
                splitter("##layout-left", {left, top}, {5, size.y - bottom - top}, true, layout.left_width);
                layout.left_width = std::clamp(layout.left_width, 160.0F, size.x * 0.3F);
            }
            if (right > 0)
            {
                float edge = -right;
                splitter("##layout-right", {size.x - right - 5, top}, {5, size.y - bottom - top}, true, edge);
                layout.right_width = std::clamp(-edge, 220.0F, size.x * 0.35F);
            }
            if (bottom > 0)
            {
                float edge = -bottom;
                splitter("##layout-bottom", {0, size.y - bottom - 5}, {size.x, 5}, false, edge);
                layout.bottom_height = std::clamp(-edge, 100.0F, size.y * 0.4F);
            }
            const float center_x = left > 0 ? left + 5 : 0;
            const float upper_height = std::max(0.0F, size.y - bottom - (bottom > 0 ? 5 : 0));
            impl_->placements = {
                {{layout.left, {0, top}, {left, std::max(0.0F, upper_height - top)}},
                 {layout.center,
                  {center_x, top},
                  {std::max(0.0F, size.x - center_x - right - (right > 0 ? 5 : 0)), std::max(0.0F, upper_height - top)}
                 },
                 {layout.right, {size.x - right, top}, {right, std::max(0.0F, upper_height - top)}},
                 {layout.bottom, {0, size.y - bottom}, {size.x, bottom}},
                 {layout.toolbar, {0, 0}, {size.x, top}}}
            };
        }
    }

    void Root::drawPane(Pane& pane) noexcept
    {
        if (!pane.visible())
        {
            if (pane.modal_)
                for (int i = 0; i < impl_->context->native()->OpenPopupStack.Size; ++i)
                    if (auto* window = impl_->context->native()->OpenPopupStack[i].Window;
                        window && std::strcmp(window->Name, pane.window_label_.c_str()) == 0)
                    {
                        ImGui::ClosePopupToLevel(i, true);
                        break;
                    }
            return;
        }
        // Give a new window a usable first-frame content region. Saved and docked
        // geometry still wins; layout never needs last frame's measured height.
        if (!ImGui::FindWindowByName(pane.window_label_.c_str()))
        {
            const auto display = ImGui::GetIO().DisplaySize;
            Size preferred{480, 320};
            if (pane.content_)
                preferred = pane.content_->measure(std::max(0.F, display.x)).preferred;
            const auto padding = ImGui::GetStyle().WindowPadding;
            ImGui::SetNextWindowSize(
                {std::min(display.x, std::max(240.F, preferred.width + padding.x * 2)),
                 std::min(display.y, std::max(160.F, preferred.height + padding.y * 2 + ImGui::GetFrameHeight()))},
                ImGuiCond_FirstUseEver
            );
        }
        ImGuiWindowFlags flags{};
        for (const auto& placement : impl_->placements)
        {
            if (pane.modal_ || placement.id.empty() || placement.id != pane.id().name())
                continue;
            if (placement.size.x <= 0 || placement.size.y <= 0)
                return;
            ImGui::SetNextWindowPos(placement.position);
            ImGui::SetNextWindowSize(placement.size);
            flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
            break;
        }
        if (!pane.modal_ && impl_->docking && impl_->split_layout)
        {
            if (auto* node = ImGui::DockBuilderGetCentralNode(ImGui::GetID("lux.ui.dockspace")))
                ImGui::SetNextWindowDockID(node->ID, ImGuiCond_FirstUseEver);
            const auto& layout = *impl_->split_layout;
            const std::array<std::string_view, 5>
                ids{layout.left, layout.center, layout.right, layout.bottom, layout.toolbar};
            for (std::size_t i{}; i < ids.size(); ++i)
            {
                if (ids[i] != pane.id().name())
                    continue;
                auto* node = ImGui::DockBuilderGetNode(impl_->dock_regions[i]);
                if (!node || !node->IsLeafNode())
                    node = ImGui::DockBuilderGetCentralNode(ImGui::GetID("lux.ui.dockspace"));
                if (node)
                    ImGui::SetNextWindowDockID(
                        node->ID,
                        impl_->reset_docking ? ImGuiCond_Always : ImGuiCond_FirstUseEver
                    );
                break;
            }
        }
        if (impl_->pending_focus == &pane)
        {
            ImGui::SetNextWindowFocus();
            impl_->pending_focus = {};
        }
        bool visible = true;
        const bool toolbar = impl_->split_layout && impl_->split_layout->toolbar == pane.id().name();
        if (toolbar && !impl_->docking)
            flags |= ImGuiWindowFlags_NoDecoration;
        if (pane.modal_ && !ImGui::IsPopupOpen(pane.window_label_.c_str()))
            ImGui::OpenPopup(pane.window_label_.c_str());
        const bool shown =
            pane.modal_
                ? ImGui::BeginPopupModal(pane.window_label_.c_str(), &visible, flags | ImGuiWindowFlags_NoDocking)
                : ImGui::Begin(pane.window_label_.c_str(), toolbar ? nullptr : &visible, flags);
        if (shown)
        {
            if (pane.modal_)
                impl_->modal = &pane;
            // Record the window before nested panes draw; a focused child wins afterwards.
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
                impl_->draw_focused = &pane;
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
                impl_->draw_hovered = &pane;
            drawPaneContent(pane);
        }
        if (pane.modal_)
        {
            if (shown)
                ImGui::EndPopup();
        }
        else
            ImGui::End();
        if (!visible)
            pane.requestClose();
    }

    Pane* Root::modalPane() const noexcept
    {
        detail::ContextActivation context{impl_->context->native()};
        const auto* modal = ImGui::GetTopMostPopupModal();
        if (!modal)
            return nullptr;
        auto* pane = impl_->modal;
        return pane && pane->visible() && pane->modal_ && std::strcmp(pane->window_label_.c_str(), modal->Name) == 0
                   ? pane
                   : nullptr;
    }

    bool Root::allowedByModal(const Pane& target) const noexcept
    {
        detail::ContextActivation context{impl_->context->native()};
        return !ImGui::GetTopMostPopupModal() || modalPane() == &target;
    }
    bool Root::allowedByModal(const Element& target) const noexcept
    {
        return allowedByModal(target.pane());
    }

    void Root::drawPaneContent(Pane& pane) noexcept
    {
        if (!impl_->drawing)
            detail::failContract();
        if (!pane.content_)
            return;
        const auto origin = ImGui::GetCursorScreenPos();
        const auto available = ImGui::GetContentRegionAvail();
        const Size size{std::max(0.F, available.x), std::max(0.F, available.y)};
        pane.content_->arrange({{}, size});
        drawElement(*pane.content_, {origin.x, origin.y});
    }

    void Root::deliverWindowFocus(bool focused_value) noexcept
    {
        const auto capture = impl_->pointer_capture;
        impl_->context->setWindowFocused(focused_value);
        VInputEvent focus = WindowFocus{focused_value};
        Impl::Target focused{impl_->focused_element};
        if (!focused)
            focused = focusedPane();
        if (!focused_value)
        {
            impl_->routed_modifiers = 0;
            if (impl_->composing)
            {
                impl_->composing = false;
                VInputEvent cancelled = Composition{ECompositionStage::CANCELLED};
                if (focused)
                    static_cast<void>(object::sendEvent(*focused, cancelled));
            }
            impl_->pointer_capture = {};
        }
        // Focus loss is an interaction-ending fact, not a command that
        // an ancestor or a modal can swallow on behalf of its owner.
        if (capture)
            static_cast<void>(object::sendEvent(*capture, focus));
        if (focused && focused != capture)
            static_cast<void>(object::sendEvent(*focused, focus));
    }

    void Root::routeInput() noexcept
    {
        const auto& io = ImGui::GetIO();
        auto* modal = modalPane();
        object::LuxObject& boundary = modal ? static_cast<object::LuxObject&>(*modal) : *this;
        // The association table stores only native identity/IME facts. ImGui's
        // queue remains the source of actual key, pointer and character events.
        const auto route = [&](const ImGuiInputEvent& input) noexcept
        {
            // An earlier handler can acquire, transfer or release capture. Resolve
            // the current owner for each event, never cache it for the whole batch.
            auto capture = impl_->pointer_capture;
            if (input.Type == ImGuiInputEventType_Focus)
            {
                deliverWindowFocus(input.AppFocused.Focused);
                if (!input.AppFocused.Focused)
                    impl_->context->cancelAdoptedInput();
                return;
            }
            if (capture && (!capture.visible() || !allowedByModal(*capture.pane())))
            {
                impl_->pointer_capture = {};
                VInputEvent cancelled = PointerCancel{};
                static_cast<void>(object::sendEvent(*capture, cancelled));
                capture = nullptr;
            }
            if (input.Type == ImGuiInputEventType_Key && (input.Key.Key & ImGuiMod_Mask_) != 0)
            {
                if (input.Key.Down)
                    impl_->routed_modifiers |= input.Key.Key;
                else
                    impl_->routed_modifiers &= ~input.Key.Key;
            }
            Impl::Target target{impl_->focused_element};
            if (!target)
                target = focusedPane();
            std::optional<VInputEvent> value;
            switch (input.Type)
            {
            case ImGuiInputEventType_Key:
                if (impl_->composing || io.WantTextInput || ImGui::IsAnyItemActive() ||
                    !ImGui::TestKeyOwner(input.Key.Key, ImGuiKeyOwner_NoOwner))
                    break;
                if (const auto key = detail::Context::keyFromNative(input.Key.Key); key != EKey::NONE)
                    value = Key{key, input.Key.Down};
                break;
            case ImGuiInputEventType_MousePos:
                target = capture ? capture : Impl::Target{impl_->hovered_element};
                if (!target)
                    target = impl_->hovered;
                if (capture || !ImGui::IsAnyItemActive())
                    value = PointerMove{{input.MousePos.PosX, input.MousePos.PosY}};
                break;
            case ImGuiInputEventType_MouseButton:
                target = capture ? capture : Impl::Target{impl_->hovered_element};
                if (!target)
                    target = impl_->hovered;
                if (input.MouseButton.Button >= 0 && input.MouseButton.Button < 3 &&
                    (capture ||
                     ImGui::TestKeyOwner(ImGui::MouseButtonToKey(input.MouseButton.Button), ImGuiKeyOwner_NoOwner)))
                {
                    const auto button = input.MouseButton.Button == 0   ? EPointerButton::LEFT
                                        : input.MouseButton.Button == 1 ? EPointerButton::RIGHT
                                                                        : EPointerButton::MIDDLE;
                    value = PointerButton{button, input.MouseButton.Down};
                }
                break;
            case ImGuiInputEventType_MouseWheel:
                target = capture ? capture : Impl::Target{impl_->hovered_element};
                if (!target)
                    target = impl_->hovered;
                if (capture ||
                    (!ImGui::IsAnyItemActive() && ImGui::TestKeyOwner(ImGuiKey_MouseWheelY, ImGuiKeyOwner_NoOwner)))
                    value = PointerWheel{{input.MouseWheel.WheelX, input.MouseWheel.WheelY}};
                break;
            default:
                break;
            }
            if (!target || !target.visible() || !allowedByModal(*target.pane()) || !value)
                return;
            if (object::routeEvent(*target, boundary, *value))
                return;
            const auto* key = std::get_if<Key>(&*value);
            if (key && !modal && impl_->shortcut(*this, *key))
                return;
            const bool control = (impl_->routed_modifiers & ImGuiMod_Ctrl) != 0;
            const bool shift = (impl_->routed_modifiers & ImGuiMod_Shift) != 0;
            const bool alternate = (impl_->routed_modifiers & (ImGuiMod_Alt | ImGuiMod_Super)) != 0;
            if (key && key->down && control && !alternate && (key->key == EKey::Z || key->key == EKey::Y))
            {
                const bool redo = key->key == EKey::Y || shift;
                Command command{CommandIdView{redo ? "lux.edit.redo" : "lux.edit.undo"}, ECommandPhase::EXECUTE};
                static_cast<void>(object::routeEvent(*target, boundary, command));
            }
        };
        auto composition = [&](ECompositionStage stage) noexcept
        {
            impl_->composing = stage == ECompositionStage::STARTED || stage == ECompositionStage::UPDATED;
            Impl::Target target{impl_->focused_element};
            if (!target)
                target = focusedPane();
            if (target && allowedByModal(*target.pane()))
            {
                VInputEvent event = Composition{stage};
                static_cast<void>(object::sendEvent(*target, event));
            }
        };
        impl_->context->consumeInput(route, composition);
        if (!ImGui::IsAnyMouseDown())
            impl_->pointer_capture = {};
    }

    lux::cxx::expected<void, EInputError> Root::feedInput(const VInputEvent& event, std::uint64_t sequence) noexcept
    {
        requireOwner();
        return impl_->context->feedInput(event, sequence);
    }

    void Root::closeInput() noexcept
    {
        if (!impl_)
            return;
        requireOwner();
        if (impl_->context->closeInput())
            deliverWindowFocus(false);
    }

    InputSnapshot Root::inputSnapshot() const noexcept
    {
        requireOwner();
        return impl_->context->inputSnapshot(impl_->composing, bool(impl_->pointer_capture));
    }

    lux::cxx::expected<PreparedDockTree, EDockError> Root::prepareDockTree(DockTree tree) const
    {
        requireOwner();
        auto invalid = [] { return cxx::unexpected(EDockError::INVALID_DATA); };
        if (tree.nodes.size() > impl_->attachment_capacity || tree.surfaces.size() > impl_->attachment_capacity)
            return invalid();
        auto prepared = std::make_unique<PreparedDockTree::Data>();
        prepared->order.reserve(tree.nodes.size());
        prepared->ids.resize(tree.nodes.size());
        std::vector<std::uint8_t> visited(tree.nodes.size());
        std::vector<std::uint32_t> pending;
        pending.reserve(tree.nodes.size());
        std::set<std::string_view> windows;
        unsigned main_surfaces{};
        for (const auto& surface : tree.surfaces)
        {
            const auto& bounds = surface.bounds;
            const bool valid_geometry = std::isfinite(bounds.position.x) && std::isfinite(bounds.position.y) &&
                                        std::isfinite(bounds.size.width) && std::isfinite(bounds.size.height) &&
                                        bounds.size.width > 0 && bounds.size.height > 0;
            if (!valid_geometry || (!surface.floating && ++main_surfaces > 1))
                return invalid();
            pending.push_back(surface.node);
            while (!pending.empty())
            {
                const auto index = pending.back();
                pending.pop_back();
                if (index >= tree.nodes.size() || visited[index])
                    return invalid();
                visited[index] = 1;
                prepared->order.push_back(index);
                const auto& node = tree.nodes[index];
                if (node.split == EDockSplit::LEAF)
                {
                    if (node.first != UINT32_MAX || node.second != UINT32_MAX)
                        return invalid();
                    for (const auto& name : node.windows)
                        if (name.empty() || name.find_first_of("\r\n") != name.npos || name.find('\0') != name.npos ||
                            !windows.insert(name).second)
                            return invalid();
                }
                else
                {
                    const bool invalid_split =
                        node.split != EDockSplit::HORIZONTAL && node.split != EDockSplit::VERTICAL;
                    const bool invalid_ratio = !std::isfinite(node.ratio) || node.ratio <= 0 || node.ratio >= 1;
                    if (invalid_split || invalid_ratio || !node.windows.empty())
                        return invalid();
                    pending.push_back(node.second);
                    pending.push_back(node.first);
                }
            }
        }
        if (prepared->order.size() != tree.nodes.size())
            return invalid();
        prepared->tree = std::move(tree);
        return PreparedDockTree{std::move(prepared)};
    }
    void Root::commitDockTree(PreparedDockTree&& prepared) noexcept
    {
        requireOwner();
        if (!prepared.data_)
            detail::failContract();
        impl_->pending_dock_tree = std::move(prepared.data_);
        ++impl_->window_revision;
    }
    DockTree Root::captureDockTree() const
    {
        requireOwner();
        detail::ContextActivation context{impl_->context->native()};
        if (impl_->pending_dock_tree)
        {
            auto result = impl_->pending_dock_tree->tree;
            std::set<std::string_view> included;
            for (const auto& node : impl_->pending_dock_tree->tree.nodes)
                for (const auto& name : node.windows)
                    included.insert(name);
            // Applying a replacement DockSpace undocks unspecified windows, without destroying
            // or hiding them. Capturing before its first draw must retain those same windows.
            for (const auto* pane : impl_->windows)
            {
                if (!pane || pane->modal_ || included.contains(pane->id().name()))
                    continue;
                const auto* window = ImGui::FindWindowByName(pane->window_label_.c_str());
                const Rect bounds = window ? Rect{{window->Pos.x, window->Pos.y}, {window->Size.x, window->Size.y}}
                                           : Rect{{40, 40}, {640, 480}};
                const auto index = static_cast<std::uint32_t>(result.nodes.size());
                result.nodes.push_back({EDockSplit::LEAF, UINT32_MAX, UINT32_MAX, .5F, {std::string(pane->id().name())}}
                );
                result.surfaces.push_back({index, bounds, true});
            }
            return result;
        }
        DockTree result;
        std::map<const ImGuiDockNode*, std::uint32_t> nodes;
        std::vector<const ImGuiDockNode*> pending;
        const auto insert = [&](const ImGuiDockNode* node)
        {
            auto [it, fresh] = nodes.emplace(node, static_cast<std::uint32_t>(result.nodes.size()));
            if (fresh)
            {
                result.nodes.emplace_back();
                pending.push_back(node);
            }
            return it->second;
        };
        for (auto* pane : impl_->windows)
        {
            if (!pane || pane->modal_)
                continue;
            const auto* window = ImGui::FindWindowByName(pane->window_label_.c_str());
            if (!window || !window->DockNode)
            {
                const auto index = static_cast<std::uint32_t>(result.nodes.size());
                result.nodes.push_back({EDockSplit::LEAF, UINT32_MAX, UINT32_MAX, .5F, {std::string(pane->id().name())}}
                );
                const Rect bounds = window ? Rect{{window->Pos.x, window->Pos.y}, {window->Size.x, window->Size.y}}
                                           : Rect{{40, 40}, {640, 480}};
                result.surfaces.push_back({index, bounds, true});
                continue;
            }
            auto* root = window->DockNode;
            while (root->ParentNode)
                root = root->ParentNode;
            if (!nodes.contains(root))
                result.surfaces.push_back(
                    {insert(root), {{root->Pos.x, root->Pos.y}, {root->Size.x, root->Size.y}}, !root->IsDockSpace()}
                );
            const auto leaf = insert(window->DockNode);
            result.nodes[leaf].windows.emplace_back(pane->id().name());
        }
        for (std::size_t i{}; i < pending.size(); ++i)
        {
            const auto* node = pending[i];
            if (!node->ChildNodes[0] || !node->ChildNodes[1])
                continue;
            const auto first = insert(node->ChildNodes[0]);
            const auto second = insert(node->ChildNodes[1]);
            auto& output = result.nodes[nodes.at(node)];
            output.split = node->SplitAxis == ImGuiAxis_X ? EDockSplit::HORIZONTAL : EDockSplit::VERTICAL;
            output.first = first;
            output.second = second;
            const auto extent = [&](const ImGuiDockNode* child)
            { return node->SplitAxis == ImGuiAxis_X ? child->Size.x : child->Size.y; };
            const auto total = extent(node->ChildNodes[0]) + extent(node->ChildNodes[1]);
            output.ratio = total > 0 ? std::clamp(extent(node->ChildNodes[0]) / total, .001F, .999F) : .5F;
        }
        return result;
    }

    DockState Root::captureDockState() const
    {
        requireOwner();
        detail::ContextActivation context{impl_->context->native()};
        std::size_t size = 0;
        const char* data = ImGui::SaveIniSettingsToMemory(&size);
        std::vector<std::byte> bytes(size);
        if (size != 0)
        {
            std::memcpy(bytes.data(), data, size);
        }
        return DockState{std::move(bytes)};
    }

    lux::cxx::expected<void, EDockError> Root::restoreDockState(
        const DockState& snapshot,
        std::span<const DockIdentity> identities
    )
    {
        requireOwner();
        const auto bytes = snapshot.bytes();
        if (bytes.empty())
        {
            return lux::cxx::unexpected<EDockError>{EDockError::INVALID_DATA};
        }
        detail::ContextActivation context{impl_->context->native()};
        std::string data(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::vector<std::pair<ImGuiID, ImGuiID>> selected;
        std::size_t position{};
        while ((position = data.find("[Window][", position)) != std::string::npos)
        {
            const auto end = data.find(']', position + 9);
            const auto marker = data.find("###", position + 9);
            if (end == std::string::npos)
                return lux::cxx::unexpected(EDockError::INVALID_DATA);
            if (marker != std::string::npos && marker < end)
            {
                const auto id = data.substr(marker + 3, end - marker - 3);
                for (const auto& mapping : identities)
                {
                    const bool matches =
                        id == mapping.saved || (id.starts_with(mapping.saved) && id.size() > mapping.saved.size() &&
                                                id[mapping.saved.size()] == '/');
                    if (!matches)
                        continue;
                    const auto replacement = mapping.current + id.substr(mapping.saved.size());
                    const auto old_label = std::string{"###"} + id;
                    const auto new_label = std::string{"###"} + replacement;
                    selected.emplace_back(ImHashStr(old_label.c_str()), ImHashStr(new_label.c_str()));
                    data.replace(marker + 3, id.size(), replacement);
                    break;
                }
            }
            position += 9;
        }
        for (const auto& [before, after] : selected)
        {
            char old_value[32], new_value[32];
            std::snprintf(old_value, sizeof(old_value), " Selected=0x%08X", before);
            std::snprintf(new_value, sizeof(new_value), " Selected=0x%08X", after);
            std::size_t at{};
            while ((at = data.find(old_value, at)) != std::string::npos)
            {
                data.replace(at, std::strlen(old_value), new_value);
                at += std::strlen(new_value);
            }
        }
        ImGui::LoadIniSettingsFromMemory(data.data(), data.size());
        if (impl_->docking && data.find("[Docking][Data]") != std::string::npos)
            impl_->dock_layout_initialized = true;
        return {};
    }

    void Root::setDockLayout(DockLayout layout)
    {
        requireOwner();
        if (!validateDockLayout(layout))
            return;
        impl_->split_layout = std::move(layout);
    }
    void Root::resetDockLayout(DockLayout layout)
    {
        checkContentChange();
        if (layout.center.empty())
        {
            if (impl_->split_layout && validateDockLayout(*impl_->split_layout))
                layout = *impl_->split_layout;
            else
                for (const auto* pane : impl_->windows)
                    if (pane && !pane->modal_)
                    {
                        layout.center = pane->id().name();
                        break;
                    }
        }
        if (!validateDockLayout(layout))
            return;
        impl_->split_layout = std::move(layout);
        impl_->dock_layout_initialized = false;
        impl_->reset_docking = true;
    }
    void Root::clearDockLayout() noexcept
    {
        requireOwner();
        impl_->split_layout.reset();
    }
    lux::cxx::expected<void, EDockError> Root::validateDockLayout(const DockLayout& layout) const noexcept
    {
        requireOwner();
        const bool valid_dimensions = std::isfinite(layout.left_width) && layout.left_width >= 0 &&
                                      std::isfinite(layout.right_width) && layout.right_width >= 0 &&
                                      std::isfinite(layout.bottom_height) && layout.bottom_height >= 0;
        if (!valid_dimensions || layout.center.empty())
            return lux::cxx::unexpected(EDockError::INVALID_DATA);
        const std::array<std::string_view, 5>
            ids{layout.left, layout.center, layout.right, layout.bottom, layout.toolbar};
        for (std::size_t i{}; i < ids.size(); ++i)
        {
            if (ids[i].empty())
                continue;
            if (std::find(ids.begin(), ids.begin() + i, ids[i]) != ids.begin() + i || !findPane(PaneIdView{ids[i]}))
                return lux::cxx::unexpected(EDockError::INVALID_DATA);
        }
        return {};
    }
} // namespace lux::ui
