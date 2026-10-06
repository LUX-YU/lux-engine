#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <imgui_internal.h>
#include <limits>
#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Command.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/detail/Context.hpp>
#include <lux/engine/ui/detail/ContextActivation.hpp>
#include <lux/engine/ui/detail/Contract.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <map>
#include <optional>
#include <unordered_set>
#include <utility>

namespace lux::ui
{
    namespace
    {
        struct Mutation final
        {
            bool& active;
            explicit Mutation(bool& flag) noexcept : active(flag)
            {
                active = true;
            }
            ~Mutation()
            {
                active = false;
            }
        };

        template <class Visit> void visitSubtree(object::LuxObject& root, Visit&& visit) noexcept
        {
            auto* node = &root;
            for (;;)
            {
                visit(*node);
                if (node->firstChild())
                {
                    node = node->firstChild();
                }
                else
                {
                    while (node != &root && !node->nextSibling())
                    {
                        node = node->parent();
                    }
                    if (node == &root)
                    {
                        return;
                    }
                    node = node->nextSibling();
                }
            }
        }

    } // namespace

    struct PreparedDockTree::Data final
    {
        object::ObjectId root;
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
        struct StoredTarget final
        {
            object::ObjectId root;
            PaneId pane;
            object::ObjectId object;
            bool operator==(const StoredTarget&) const noexcept = default;
        };
        static StoredTarget store(Root&, Pane&, object::LuxObject&) noexcept;
        static object::LuxObject* resolve(Root&, StoredTarget) noexcept;
        void queueChange(StoredTarget, ChangeCallback) noexcept;
        void cancelChanges(object::LuxObject&) noexcept;
        void applyPendingChanges(Root&) noexcept;
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
            StoredTarget target;
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
        Pane *focused{}, *hovered{}, *pending_focus{};
        Target pointer_capture;
        Pane *draw_focused{}, *draw_hovered{};
        Element *focused_element{}, *hovered_element{}, *pending_element{};
        Element *draw_focused_element{}, *draw_hovered_element{};
        std::size_t pane_capacity{65536};
        cxx::SlotKeyAutoSparseSet<PaneId, std::unique_ptr<Pane>> panes;
        struct Change final
        {
            StoredTarget target;
            ChangeCallback apply{};
            bool operator==(const Change&) const noexcept = default;
        };
        std::vector<Change> changes;
        std::size_t change_batch_size{};
        object::LuxObject* active_change{};
        object::LuxObject* active_update{};
        std::size_t layout_depth{};
        std::uint64_t layout_epoch{};
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
            PaneId id;
            ImVec2 position, size;
        };
        std::array<Placement, 5> placements{};
    };

    Root::Root() noexcept : LuxObject() {}
    Root::~Root() noexcept
    {
        if (!isOnAffinityThread())
        {
            detail::failContract();
        }
        if (impl_ && !clearPanes())
        {
            detail::failContract();
        }
        beginDestruction();
    }

    Root::CreateResult Root::create(RootConfig config) noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            return lux::cxx::unexpected(EInitError::WRONG_THREAD);
        }
        {
            auto root = std::unique_ptr<Root>(new Root());
            auto initialized = root->initialize(config);
            if (!initialized)
            {
                return lux::cxx::unexpected(initialized.error());
            }
            return root;
        }
    }

    lux::cxx::expected<void, EInitError> Root::initialize(RootConfig config) noexcept
    {
        if (!isOnAffinityThread())
        {
            return cxx::unexpected(EInitError::WRONG_THREAD);
        }
        if (impl_)
        {
            detail::failContract();
        }
        auto context = detail::Context::create(config);
        if (!context)
        {
            return cxx::unexpected(context.error());
        }
        auto data = std::make_unique<Impl>();
        data->context = std::move(*context);
        data->pane_capacity = config.pane_capacity;
        data->docking = config.docking;
        impl_ = std::move(data);
        return {};
    }

    void Root::requireOwner() const noexcept
    {
        if (!impl_ || !isOnAffinityThread())
        {
            detail::failContract();
        }
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
    Pane* Root::findPane(PaneId id) const noexcept
    {
        requireOwner();
        auto* owner = impl_->panes.tryGet(id);
        return owner ? owner->get() : nullptr;
    }

    PaneResult<void> Root::withPane(object::ObjectId root, PaneId id, cxx::function_ref<void(Pane&)> visit) noexcept
    {
        auto ready = checkStructureSafe();
        if (!ready)
        {
            return ready;
        }
        if (root != objectId())
        {
            return cxx::unexpected(EPaneError::INVALID_ID);
        }
        auto* pane = findPane(id);
        if (!pane)
        {
            return cxx::unexpected(EPaneError::INVALID_ID);
        }
        Mutation guard{impl_->updating};
        beginCallbackBorrow(*this);
        beginCallbackBorrow(*pane);
        visit(*pane);
        endCallbackBorrow(*pane);
        endCallbackBorrow(*this);
        return {};
    }

    void Root::showPanes() noexcept
    {
        requireOwner();
        beginTreeVisit();
        for (const auto& pane_owner : impl_->panes.values())
        {
            auto* pane = pane_owner.get();

            if (!pane->modal())
            {
                pane->setVisible(true);
            }
        }
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
        const bool can_focus = pane.attachedRoot() == this && pane.visible() && allowedByModal(pane);
        if (!can_focus)
        {
            return false;
        }
        impl_->pending_focus = &pane;
        return true;
    }
    bool Root::requestFocus(PaneId id) noexcept
    {
        auto* pane = findPane(id);
        return pane && requestFocus(*pane);
    }
    bool Root::capturePointer(Pane& pane) noexcept
    {
        requireOwner();
        const bool can_capture =
            pane.attachedRoot() == this && pane.visible() && impl_->context->windowFocused() && allowedByModal(pane);
        if (!can_capture)
        {
            return false;
        }
        impl_->pointer_capture = &pane;
        return true;
    }
    void Root::releasePointer(Pane& pane) noexcept
    {
        requireOwner();
        if (impl_->pointer_capture.window == &pane)
        {
            impl_->pointer_capture = {};
        }
    }
    void Root::bindWindow(window::LuxWindow* window) noexcept
    {
        if (!impl_ && !window)
        {
            return; // Detaching is valid after failed initialization.
        }
        requireOwner();
        if (!impl_)
        {
            return;
        }
        impl_->window = window;
        impl_->context->bindWindow(window ? window->nativeHandle() : nullptr);
    }
    window::LuxWindow* Root::window() const noexcept
    {
        requireOwner();
        return impl_ ? impl_->window : nullptr;
    }

    lux::cxx::expected<void, ECaptureError> Root::update(
        FrameInfo info,
        DrawData* output,
        std::optional<Capture> capture
    ) noexcept
    {
        requireOwner();
        const bool is_active_visit = impl_->drawing || impl_->updating || impl_->layout_depth != 0;
        const bool is_active_callback = impl_->change_batch_size != 0 || impl_->active_change || isDispatching();
        if (is_active_visit || is_active_callback)
        {
            return lux::cxx::unexpected(ECaptureError::FRAME_OPEN);
        }
        lux::cxx::expected<void, ECaptureError> result;
        if (output)
        {
            result = collectDrawData(info, *output);
            if (result)
            {
                // Freeze mutation until all references in the captured data have been pinned.
                impl_->updating = true;
                if (capture)
                {
                    result = (*capture)(*output);
                }
                impl_->updating = false;
            }
        }
        // Input and accepted interactions still finish if capture/pinning failed.
        maintain();
        return result;
    }

    void Root::deferChange(Pane& target, ChangeCallback apply) noexcept
    {
        requireOwner();
        if (target.attachedRoot() != this)
        {
            detail::failContract();
        }
        impl_->queueChange(Impl::store(*this, target, target), apply);
    }

    void Root::deferChange(Element& target, ChangeCallback apply) noexcept
    {
        requireOwner();
        if (target.attachedRoot() != this)
        {
            detail::failContract();
        }
        impl_->queueChange(Impl::store(*this, target.pane(), target), apply);
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
    std::span<const std::unique_ptr<Pane>> Root::panes() const noexcept
    {
        requireOwner();
        return impl_->panes.values();
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
        {
            return;
        }
        if (command.phase == ECommandPhase::QUERY)
        {
            static_cast<void>(object::routeEvent(*target, root, command));
        }
        else
        {
            menu_calls.push_back({store(root, *menu_pane, *target), CommandId{std::string(command.id.name())}});
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
                {
                    ImGui::Separator();
                }
                else
                {
                    ImGui::TextDisabled("%s", label);
                }
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
            {
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
        {
            return false;
        }
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
                {
                    return &item;
                }
                if (const auto* nested = self(self, item.children))
                {
                    return nested;
                }
            }
            return nullptr;
        };
        const auto* item = find(find, menu);
        if (!item)
        {
            return false;
        }
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

    Root::Impl::StoredTarget Root::Impl::store(Root& root, Pane& pane, object::LuxObject& target) noexcept
    {
        return {root.objectId(), pane.id(), target.objectId()};
    }

    object::LuxObject* Root::Impl::resolve(Root& root, StoredTarget target) noexcept
    {
        if (target.root != root.objectId())
        {
            return nullptr;
        }
        const auto* pane = root.findPane(target.pane);
        if (!pane)
        {
            return nullptr;
        }
        auto resolved = object::ObjectRuntime::instance().resolve(target.object);
        if (!resolved)
        {
            return nullptr;
        }
        for (auto* ancestor = *resolved; ancestor; ancestor = ancestor->parent())
        {
            if (ancestor == pane)
            {
                return *resolved;
            }
        }
        return nullptr;
    }

    void Root::Impl::queueChange(StoredTarget target, ChangeCallback apply) noexcept
    {
        if (!apply)
        {
            detail::failContract();
        }
        const Change change{target, apply};
        const auto pending = changes.begin() + change_batch_size;
        if (std::find(pending, changes.end(), change) == changes.end())
        {
            changes.push_back(change);
        }
    }

    void Root::Impl::cancelChanges(object::LuxObject& target) noexcept
    {
        for (auto& call : menu_calls)
        {
            if (call.target.object == target.objectId())
            {
                call.target = {};
            }
        }
        // The active batch keeps its indices even if a preceding callback destroys a later target.
        for (std::size_t index{}; index < change_batch_size; ++index)
        {
            if (changes[index].target.object == target.objectId())
            {
                changes[index] = {};
            }
        }
        const auto pending = changes.begin() + change_batch_size;
        const auto end = std::remove_if(
            pending,
            changes.end(),
            [&target](const Change& change) noexcept { return change.target.object == target.objectId(); }
        );
        changes.erase(end, changes.end());
    }

    void Root::applyPendingChanges() noexcept
    {
        requireOwner();
        impl_->applyPendingChanges(*this);
        const auto count = impl_->menu_calls.size();
        for (std::size_t index{}; index < count; ++index)
        {
            const auto call = impl_->menu_calls[index];
            auto* target = Impl::resolve(*this, call.target);
            if (!target)
            {
                continue;
            }
            Command command{call.command.view()};
            static_cast<void>(object::routeEvent(*target, *this, command));
            if (command.enabled)
            {
                command.phase = ECommandPhase::EXECUTE;
                static_cast<void>(object::routeEvent(*target, *this, command));
            }
        }
        impl_->menu_calls.erase(impl_->menu_calls.begin(), impl_->menu_calls.begin() + count);
    }

    bool Root::hasPendingChanges() const noexcept
    {
        requireOwner();
        return !impl_->changes.empty() || !impl_->menu_calls.empty();
    }

    void Root::Impl::applyPendingChanges(Root& root) noexcept
    {
        const bool is_active_visit = drawing || updating || layout_depth != 0;
        const bool is_active_callback = change_batch_size != 0 || active_change || Root::isDispatching();
        if (is_active_visit || is_active_callback)
        {
            detail::failContract();
        }
        change_batch_size = changes.size();
        for (std::size_t index{}; index < change_batch_size; ++index)
        {
            // No vector reference survives the call: it may enqueue or destroy other targets.
            const auto change = std::exchange(changes[index], Change{});
            auto* target = resolve(root, change.target);
            if (!target)
            {
                continue;
            }
            active_change = target;
            Root::beginCallbackBorrow(*target);
            change.apply(*target);
            Root::endCallbackBorrow(*target);
            active_change = nullptr;
        }
        changes.erase(changes.begin(), changes.begin() + change_batch_size);
        change_batch_size = 0;
    }

    lux::cxx::expected<void, ECaptureError> Root::collectDrawData(FrameInfo info, DrawData& output) noexcept
    {
        requireOwner();
        if (impl_->drawing || impl_->updating)
        {
            return lux::cxx::unexpected(ECaptureError::FRAME_OPEN);
        }
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
        {
            return started;
        }
        ++impl_->layout_epoch;
        impl_->draw_focused = impl_->draw_hovered = nullptr;
        impl_->draw_focused_element = impl_->draw_hovered_element = nullptr;
        impl_->drawMenu(*this);
        prepareLayout();
        for (const auto& pane_owner : impl_->panes.values())
        {
            auto* pane = pane_owner.get();

            if (pane)
            {
                drawPane(*pane);
            }
        }
        impl_->focused_element = impl_->draw_focused_element;
        impl_->hovered_element = impl_->draw_hovered_element;
        auto* old_focus = impl_->focused;
        auto* old_hover = impl_->hovered;
        if (old_focus != impl_->draw_focused)
        {
            impl_->focused = impl_->draw_focused;
            if (old_focus)
            {
                old_focus->setFocused(false);
            }
            if (impl_->draw_focused)
            {
                impl_->draw_focused->setFocused(true);
            }
        }
        if (old_hover)
        {
            old_hover->setHovered(false);
        }
        impl_->hovered = impl_->draw_hovered;
        if (impl_->draw_hovered)
        {
            impl_->draw_hovered->setHovered(true);
        }
        impl_->reset_docking = false;
        return impl_->context->endFrame(output);
    }

    bool Root::attachmentSafe() const noexcept
    {
        return !impl_->drawing && !impl_->updating && impl_->layout_depth == 0 && !impl_->committing_structure &&
               !isDispatching();
    }

    PaneResult<void> Root::checkStructureSafe() const noexcept
    {
        if (!isOnAffinityThread())
        {
            return cxx::unexpected(EPaneError::WRONG_THREAD);
        }
        if (isClosing())
        {
            return cxx::unexpected(EPaneError::CLOSED);
        }
        if (!attachmentSafe())
        {
            return cxx::unexpected(EPaneError::BUSY);
        }
        return {};
    }

    PaneResult<std::reference_wrapper<Pane>> Root::addPane(std::unique_ptr<Pane>&& candidate) noexcept
    {
        auto* pane = candidate.get();
        auto result = addPanes(std::span{&candidate, 1});
        if (!result)
        {
            return cxx::unexpected(result.error());
        }
        return std::ref(*pane);
    }

    PaneResult<void> Root::addPanes(std::span<std::unique_ptr<Pane>> candidates) noexcept
    {
        auto ready = checkStructureSafe();
        if (!ready)
        {
            return ready;
        }
        const auto available = impl_->pane_capacity - std::min(impl_->panes.size(), impl_->pane_capacity);
        if (candidates.size() > available)
        {
            return cxx::unexpected(EPaneError::CAPACITY);
        }
        std::vector<Pane*> prepared;
        prepared.reserve(candidates.size());
        for (const auto& candidate : candidates)
        {
            if (!candidate)
            {
                return cxx::unexpected(EPaneError::INVALID_TREE);
            }
            const bool already_attached = candidate->root_ || candidate->parent() || candidate->id_.isValid();
            if (already_attached)
            {
                return cxx::unexpected(EPaneError::ALREADY_ATTACHED);
            }
            if (std::ranges::find(prepared, candidate.get()) != prepared.end())
            {
                return cxx::unexpected(EPaneError::DUPLICATE_ID);
            }
            auto relation = validateRelation(*candidate, this);
            if (!relation)
            {
                return cxx::unexpected(
                    relation.error() == object::EObjectTreeError::CLOSED ? EPaneError::CLOSED : EPaneError::BUSY
                );
            }
            prepared.push_back(candidate.get());
        }
        if (!impl_->panes.prepareInsert(candidates.size()))
        {
            return cxx::unexpected(EPaneError::CAPACITY);
        }
        for (auto* pane : prepared)
        {
            pane->window_label_.reserve(pane->title_.size() + 64);
        }
        Mutation commit{impl_->committing_structure};
        for (std::size_t index{}; index < candidates.size(); ++index)
        {
            auto* pane = prepared[index];
            pane->id_ = impl_->panes.insert(std::move(candidates[index]));
            pane->root_ = this;
            pane->rebuildWindowLabel();
            commitRelation(*pane, this);
        }
        if (!prepared.empty())
        {
            ++impl_->window_revision;
        }
        for (auto* pane : prepared)
        {
            pane->beginTreeVisit();
            static_cast<void>(emit(paneChanged, PaneChanged{pane->id_, true}));
            pane->endTreeVisit();
        }
        return {};
    }

    PaneResult<std::unique_ptr<Pane>> Root::removePane(Pane& pane) noexcept
    {
        auto ready = checkStructureSafe();
        if (!ready)
        {
            return cxx::unexpected(ready.error());
        }
        if (pane.root_ != this || findPane(pane.id_) != &pane)
        {
            return cxx::unexpected(EPaneError::NOT_ATTACHED);
        }
        if (!validateRelation(pane, nullptr))
        {
            return cxx::unexpected(EPaneError::BUSY);
        }
        Mutation commit{impl_->committing_structure};
        const auto id = pane.id_;
        releasePane(pane);
        commitRelation(pane, nullptr);
        pane.root_ = nullptr;
        pane.id_ = {};
        std::unique_ptr<Pane> owner;
        impl_->panes.extract(id, owner); // Move before swap-and-pop; no user destructor runs in the container.
        ++impl_->window_revision;
        notifyRemoved(pane);
        static_cast<void>(emit(paneChanged, PaneChanged{id, false}));
        return owner;
    }

    PaneResult<void> Root::clearPanes() noexcept
    {
        auto ready = checkStructureSafe();
        if (!ready)
        {
            return ready;
        }
        for (const auto& owner : impl_->panes.values())
        {
            if (!validateRelation(*owner, nullptr))
            {
                return cxx::unexpected(EPaneError::BUSY);
            }
        }
        struct Removed final
        {
            PaneId id;
            std::unique_ptr<Pane> owner;
        };
        std::vector<Removed> removed(impl_->panes.size());
        Mutation commit{impl_->committing_structure};
        for (auto& entry : removed)
        {
            entry.id = impl_->panes.keys().back();
            auto& pane = **impl_->panes.tryGet(entry.id);
            releasePane(pane);
            commitRelation(pane, nullptr);
            pane.root_ = nullptr;
            pane.id_ = {};
            impl_->panes.extract(entry.id, entry.owner);
        }
        if (!removed.empty())
        {
            ++impl_->window_revision;
        }
        for (auto& entry : removed)
        {
            notifyRemoved(*entry.owner);
            static_cast<void>(emit(paneChanged, PaneChanged{entry.id, false}));
        }
        // Every registration has gone before notification or destruction can observe the Root.
        removed.clear();
        return {};
    }

    PaneResult<void> Root::compose(
        Pane* pane,
        Element* element,
        Element& child,
        Element* previous,
        bool replace
    ) noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            return cxx::unexpected(EPaneError::WRONG_THREAD);
        }
        auto& parent = pane ? static_cast<object::LuxObject&>(*pane) : *element;
        auto* root = pane ? pane->root_ : element->attachedRoot();
        const bool is_busy = isDispatching() || (root && !root->attachmentSafe());
        if (is_busy)
        {
            return cxx::unexpected(EPaneError::BUSY);
        }
        if (previous && previous->parent() != &parent)
        {
            return cxx::unexpected(EPaneError::NOT_ATTACHED);
        }
        if (previous && (!replace || previous == &child))
        {
            return cxx::unexpected(EPaneError::OCCUPIED);
        }
        if (child.parent())
        {
            return cxx::unexpected(EPaneError::ALREADY_ATTACHED);
        }
        auto valid = validateRelation(child, &parent);
        if (!valid)
        {
            using enum object::EObjectTreeError;
            if (valid.error() == CLOSED)
            {
                return cxx::unexpected(EPaneError::CLOSED);
            }
            return cxx::unexpected(valid.error() == BUSY ? EPaneError::BUSY : EPaneError::INVALID_TREE);
        }
        if (previous && !validateRelation(*previous, nullptr))
        {
            return cxx::unexpected(EPaneError::BUSY);
        }
        for (auto* node = parent.firstChild(); node; node = node->nextSibling())
        {
            if (node != previous && static_cast<Element*>(node)->id().view() == child.id().view())
            {
                return cxx::unexpected(EPaneError::DUPLICATE_ID);
            }
        }
        bool detached_mutation{};
        Mutation commit{root ? root->impl_->committing_structure : detached_mutation};
        if (previous)
        {
            if (root)
            {
                root->releaseElement(*previous);
            }
            commitRelation(*previous, nullptr);
            previous->assignPane(nullptr);
            previous->element_parent_ = nullptr;
        }
        commitRelation(child, &parent);
        child.element_parent_ = element;
        child.assignPane(pane ? pane : element->pane_);
        if (pane)
        {
            pane->content_ = &child;
        }
        if (root)
        {
            ++root->impl_->layout_epoch;
            if (previous)
            {
                root->notifyRemoved(*previous);
            }
        }
        return {};
    }

    void Root::notifyRemoved(object::LuxObject& subtree) noexcept
    {
        beginCallbackBorrow(subtree);
        visitSubtree(subtree, [&](object::LuxObject& node) noexcept { static_cast<void>(emit(objectRemoved, &node)); });
        endCallbackBorrow(subtree);
    }

    void Root::paneLabelChanged() noexcept
    {
        requireOwner();
        ++impl_->window_revision;
    }

    void Root::releasePane(Pane& pane) noexcept
    {
        if (pane.content_)
        {
            releaseElement(*pane.content_);
        }
        impl_->pending_dock_tree.reset();
        if (impl_->menu_pane == &pane)
        {
            impl_->menu_pane = nullptr;
            impl_->menu_element = nullptr;
        }
        ++impl_->window_revision;
        impl_->cancelChanges(pane);
        for (auto** target :
             {&impl_->focused,
              &impl_->hovered,
              &impl_->pending_focus,
              &impl_->draw_focused,
              &impl_->draw_hovered,
              &impl_->modal})
        {
            if (*target == &pane)
            {
                *target = nullptr;
            }
        }
        if (impl_->pointer_capture.window == &pane)
        {
            impl_->pointer_capture = {};
        }
        pane.focused_ = pane.hovered_ = false;
        pane.close_requested_ = false;
    }

    void Root::releaseElement(Element& subtree) noexcept
    {
        visitSubtree(
            subtree,
            [&](object::LuxObject& node) noexcept
            {
                auto& element = static_cast<Element&>(node);
                if (impl_->menu_element == &element)
                {
                    impl_->menu_element = nullptr;
                }
                impl_->cancelChanges(element);
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
                {
                    if (*target == &element)
                    {
                        *target = nullptr;
                    }
                }
                if (impl_->pointer_capture.element == &element)
                {
                    impl_->pointer_capture = {};
                }
                element.hovered_ = false;
            }
        );
    }

    void Root::checkDestruction(const object::LuxObject& object) const noexcept
    {
        requireOwner();
        for (auto* callback : {impl_->active_update, impl_->active_change})
        {
            for (auto* active = callback; active; active = active->parent())
            {
                if (active == &object)
                {
                    detail::failContract();
                }
            }
        }
    }

    void Root::checkContentChange() const noexcept
    {
        requireOwner();
        if (!attachmentSafe())
        {
            detail::failContract();
        }
    }

    void Root::maintain() noexcept
    {
        requireOwner();
        if (impl_->drawing || impl_->updating)
        {
            detail::failContract();
        }
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
        // The hierarchy is frozen for this whole pass; no global Element index or per-frame snapshot.
        for (const auto& owner : impl_->panes.values())
        {
            auto& pane = *owner;
            impl_->active_update = &pane;
            beginCallbackBorrow(pane);
            pane.update();
            endCallbackBorrow(pane);
            if (pane.content_)
            {
                visitSubtree(
                    *pane.content_,
                    [&](object::LuxObject& node) noexcept
                    {
                        auto& element = static_cast<Element&>(node);
                        impl_->active_update = &element;
                        beginCallbackBorrow(element);
                        element.update();
                        endCallbackBorrow(element);
                    }
                );
            }
        }
        impl_->active_update = nullptr;
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
        {
            return false;
        }
        impl_->pending_element = &element;
        return requestFocus(element.pane());
    }
    bool Root::capturePointer(Element& element) noexcept
    {
        requireOwner();
        if (element.attachedRoot() != this || !element.displayed() || !element.enabled() ||
            !impl_->context->windowFocused() || !allowedByModal(element))
        {
            return false;
        }
        impl_->pointer_capture = &element;
        return true;
    }
    void Root::releaseFocus(Element& element) noexcept
    {
        requireOwner();
        if (element.attachedRoot() != this)
        {
            detail::failContract();
        }
        if (impl_->pending_element == &element)
        {
            impl_->pending_element = {};
        }
        if (impl_->focused_element != &element)
        {
            return;
        }
        detail::ContextActivation context{impl_->context->native()};
        ImGui::ClearActiveID();
        impl_->focused_element = {};
        releasePointer(element);
    }
    void Root::releasePointer(Element& element) noexcept
    {
        requireOwner();
        if (impl_->pointer_capture.element == &element)
        {
            impl_->pointer_capture = {};
        }
    }
    SizeHint Root::measureElement(Element& element, float width, bool intrinsic) noexcept
    {
        requireOwner();
        detail::ContextActivation context{impl_->context->native()};
        if (impl_->layout_depth++ == 0 && !impl_->drawing)
        {
            ++impl_->layout_epoch;
        }
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
        {
            ++impl_->layout_epoch;
        }
        static_cast<void>(element.measure(element.rect().size.width));
        element.arrangeContent();
        --impl_->layout_depth;
    }
    void Root::drawElement(Element& element, Point parent_origin) noexcept
    {
        if (!impl_->drawing || element.attachedRoot() != this)
        {
            detail::failContract();
        }
        if (!element.visible_)
        {
            return;
        }
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
        {
            ImGui::SetKeyboardFocusHere();
        }
        element.draw();
        ImGui::EndGroup();
        element.hovered_ = ImGui::IsItemHovered();
        const bool clicked =
            element.hovered_ && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::IsMouseClicked(2));
        const bool retained = focusedElement() == &element && ImGui::IsWindowFocused();
        if (impl_->draw_hovered_element == prior_hover && element.hovered_)
        {
            impl_->draw_hovered_element = &element;
        }
        if (impl_->draw_focused_element == prior_focus && element.enabled() &&
            (requested || clicked || retained || ImGui::IsItemFocused()))
        {
            impl_->draw_focused_element = &element;
        }
        if (requested)
        {
            impl_->pending_element = {};
        }
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
                    {
                        ImGui::DockBuilderRemoveNode(root);
                    }
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
                    {
                        ImGui::DockBuilderSplitNode(
                            id,
                            node.split == EDockSplit::HORIZONTAL ? ImGuiDir_Left : ImGuiDir_Up,
                            node.ratio,
                            &prepared->ids[node.first],
                            &prepared->ids[node.second]
                        );
                    }
                    else
                    {
                        for (const auto& window : node.windows)
                        {
                            if (auto* pane = findPane(window))
                            {
                                ImGui::DockBuilderDockWindow(pane->window_label_.c_str(), id);
                            }
                        }
                    }
                }
                for (const auto& surface : tree.surfaces)
                {
                    ImGui::DockBuilderFinish(prepared->ids[surface.node]);
                }
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
                if (layout.toolbar.isValid())
                {
                    ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.12F, &regions[4], &center);
                }
                if (layout.bottom.isValid())
                {
                    ImGui::DockBuilderSplitNode(
                        center,
                        ImGuiDir_Down,
                        std::clamp(layout.bottom_height / viewport->WorkSize.y, 0.1F, 0.4F),
                        &regions[3],
                        &center
                    );
                }
                if (layout.left.isValid())
                {
                    ImGui::DockBuilderSplitNode(
                        center,
                        ImGuiDir_Left,
                        std::clamp(layout.left_width / viewport->WorkSize.x, 0.1F, 0.3F),
                        &regions[0],
                        &center
                    );
                }
                if (layout.right.isValid())
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
                {
                    for (const auto& pane_owner : impl_->panes.values())
                    {
                        auto* pane = pane_owner.get();

                        if (!pane->modal_)
                        {
                            ImGui::DockBuilderDockWindow(pane->window_label_.c_str(), center);
                        }
                    }
                }
                ImGui::DockBuilderFinish(root);
            }
            // Submit even when panes are hidden: docking owns their persistent placement.
            ImGui::DockSpaceOverViewport(root, viewport);
        }
        if (impl_->split_layout && !impl_->docking)
        {
            auto& layout = *impl_->split_layout;
            const auto size = ImGui::GetIO().DisplaySize;
            const float top = impl_->menu_height + (!layout.toolbar.isValid() ? 0 : 38.0F);
            const auto visible = [&](PaneId id)
            {
                const auto* pane = findPane(id);
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
            {
                for (int i = 0; i < impl_->context->native()->OpenPopupStack.Size; ++i)
                {
                    if (auto* window = impl_->context->native()->OpenPopupStack[i].Window;
                        window && std::strcmp(window->Name, pane.window_label_.c_str()) == 0)
                    {
                        ImGui::ClosePopupToLevel(i, true);
                        break;
                    }
                }
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
            {
                preferred = pane.content_->measure(std::max(0.F, display.x)).preferred;
            }
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
            if (pane.modal_ || !placement.id.isValid() || placement.id != pane.id())
            {
                continue;
            }
            if (placement.size.x <= 0 || placement.size.y <= 0)
            {
                return;
            }
            ImGui::SetNextWindowPos(placement.position);
            ImGui::SetNextWindowSize(placement.size);
            flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
            break;
        }
        if (!pane.modal_ && impl_->docking && impl_->split_layout)
        {
            if (auto* node = ImGui::DockBuilderGetCentralNode(ImGui::GetID("lux.ui.dockspace")))
            {
                ImGui::SetNextWindowDockID(node->ID, ImGuiCond_FirstUseEver);
            }
            const auto& layout = *impl_->split_layout;
            const std::array<PaneId, 5> ids{layout.left, layout.center, layout.right, layout.bottom, layout.toolbar};
            for (std::size_t i{}; i < ids.size(); ++i)
            {
                if (ids[i] != pane.id())
                {
                    continue;
                }
                auto* node = ImGui::DockBuilderGetNode(impl_->dock_regions[i]);
                if (!node || !node->IsLeafNode())
                {
                    node = ImGui::DockBuilderGetCentralNode(ImGui::GetID("lux.ui.dockspace"));
                }
                if (node)
                {
                    ImGui::SetNextWindowDockID(
                        node->ID,
                        impl_->reset_docking ? ImGuiCond_Always : ImGuiCond_FirstUseEver
                    );
                }
                break;
            }
        }
        if (impl_->pending_focus == &pane)
        {
            ImGui::SetNextWindowFocus();
            impl_->pending_focus = {};
        }
        bool visible = true;
        const bool toolbar = impl_->split_layout && impl_->split_layout->toolbar == pane.id();
        if (toolbar && !impl_->docking)
        {
            flags |= ImGuiWindowFlags_NoDecoration;
        }
        if (pane.modal_ && !ImGui::IsPopupOpen(pane.window_label_.c_str()))
        {
            ImGui::OpenPopup(pane.window_label_.c_str());
        }
        const bool shown =
            pane.modal_
                ? ImGui::BeginPopupModal(pane.window_label_.c_str(), &visible, flags | ImGuiWindowFlags_NoDocking)
                : ImGui::Begin(pane.window_label_.c_str(), toolbar ? nullptr : &visible, flags);
        if (shown)
        {
            if (pane.modal_)
            {
                impl_->modal = &pane;
            }
            // Record the window before nested panes draw; a focused child wins afterwards.
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            {
                impl_->draw_focused = &pane;
            }
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
            {
                impl_->draw_hovered = &pane;
            }
            drawPaneContent(pane);
        }
        if (pane.modal_)
        {
            if (shown)
            {
                ImGui::EndPopup();
            }
        }
        else
        {
            ImGui::End();
        }
        if (!visible)
        {
            pane.requestClose();
        }
    }

    Pane* Root::modalPane() const noexcept
    {
        detail::ContextActivation context{impl_->context->native()};
        const auto* modal = ImGui::GetTopMostPopupModal();
        if (!modal)
        {
            return nullptr;
        }
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
        {
            detail::failContract();
        }
        if (!pane.content_)
        {
            return;
        }
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
        {
            focused = focusedPane();
        }
        if (!focused_value)
        {
            impl_->routed_modifiers = 0;
            if (impl_->composing)
            {
                impl_->composing = false;
                VInputEvent cancelled = Composition{ECompositionStage::CANCELLED};
                if (focused)
                {
                    static_cast<void>(object::sendEvent(*focused, cancelled));
                }
            }
            impl_->pointer_capture = {};
        }
        // Focus loss is an interaction-ending fact, not a command that
        // an ancestor or a modal can swallow on behalf of its owner.
        if (capture)
        {
            static_cast<void>(object::sendEvent(*capture, focus));
        }
        if (focused && focused != capture)
        {
            static_cast<void>(object::sendEvent(*focused, focus));
        }
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
                {
                    impl_->context->cancelAdoptedInput();
                }
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
                {
                    impl_->routed_modifiers |= input.Key.Key;
                }
                else
                {
                    impl_->routed_modifiers &= ~input.Key.Key;
                }
            }
            Impl::Target target{impl_->focused_element};
            if (!target)
            {
                target = focusedPane();
            }
            std::optional<VInputEvent> value;
            switch (input.Type)
            {
            case ImGuiInputEventType_Key:
                if (impl_->composing || io.WantTextInput || ImGui::IsAnyItemActive() ||
                    !ImGui::TestKeyOwner(input.Key.Key, ImGuiKeyOwner_NoOwner))
                {
                    break;
                }
                if (const auto key = detail::Context::keyFromNative(input.Key.Key); key != EKey::NONE)
                {
                    value = Key{key, input.Key.Down};
                }
                break;
            case ImGuiInputEventType_MousePos:
                target = capture ? capture : Impl::Target{impl_->hovered_element};
                if (!target)
                {
                    target = impl_->hovered;
                }
                if (capture || !ImGui::IsAnyItemActive())
                {
                    value = PointerMove{{input.MousePos.PosX, input.MousePos.PosY}};
                }
                break;
            case ImGuiInputEventType_MouseButton:
                target = capture ? capture : Impl::Target{impl_->hovered_element};
                if (!target)
                {
                    target = impl_->hovered;
                }
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
                {
                    target = impl_->hovered;
                }
                if (capture ||
                    (!ImGui::IsAnyItemActive() && ImGui::TestKeyOwner(ImGuiKey_MouseWheelY, ImGuiKeyOwner_NoOwner)))
                {
                    value = PointerWheel{{input.MouseWheel.WheelX, input.MouseWheel.WheelY}};
                }
                break;
            default:
                break;
            }
            if (!target || !target.visible() || !allowedByModal(*target.pane()) || !value)
            {
                return;
            }
            if (object::routeEvent(*target, boundary, *value))
            {
                return;
            }
            const auto* key = std::get_if<Key>(&*value);
            if (key && !modal && impl_->shortcut(*this, *key))
            {
                return;
            }
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
            {
                target = focusedPane();
            }
            if (target && allowedByModal(*target.pane()))
            {
                VInputEvent event = Composition{stage};
                static_cast<void>(object::sendEvent(*target, event));
            }
        };
        impl_->context->consumeInput(route, composition);
        if (!ImGui::IsAnyMouseDown())
        {
            impl_->pointer_capture = {};
        }
    }

    lux::cxx::expected<void, EInputError> Root::feedInput(const VInputEvent& event, std::uint64_t sequence) noexcept
    {
        requireOwner();
        return impl_->context->feedInput(event, sequence);
    }

    void Root::closeInput() noexcept
    {
        if (!impl_)
        {
            return;
        }
        requireOwner();
        if (impl_->context->closeInput())
        {
            deliverWindowFocus(false);
        }
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
        if (tree.nodes.size() > impl_->pane_capacity || tree.surfaces.size() > impl_->pane_capacity)
        {
            return invalid();
        }
        auto prepared = std::make_unique<PreparedDockTree::Data>();
        prepared->order.reserve(tree.nodes.size());
        prepared->ids.resize(tree.nodes.size());
        std::vector<std::uint8_t> visited(tree.nodes.size());
        std::vector<std::uint32_t> pending;
        pending.reserve(tree.nodes.size());
        std::unordered_set<PaneId> windows;
        unsigned main_surfaces{};
        for (const auto& surface : tree.surfaces)
        {
            const auto& bounds = surface.bounds;
            const bool valid_geometry = std::isfinite(bounds.position.x) && std::isfinite(bounds.position.y) &&
                                        std::isfinite(bounds.size.width) && std::isfinite(bounds.size.height) &&
                                        bounds.size.width > 0 && bounds.size.height > 0;
            if (!valid_geometry || (!surface.floating && ++main_surfaces > 1))
            {
                return invalid();
            }
            pending.push_back(surface.node);
            while (!pending.empty())
            {
                const auto index = pending.back();
                pending.pop_back();
                if (index >= tree.nodes.size() || visited[index])
                {
                    return invalid();
                }
                visited[index] = 1;
                prepared->order.push_back(index);
                const auto& node = tree.nodes[index];
                if (node.split == EDockSplit::LEAF)
                {
                    if (node.first != UINT32_MAX || node.second != UINT32_MAX)
                    {
                        return invalid();
                    }
                    for (const auto id : node.windows)
                    {
                        const auto* pane = findPane(id);
                        if (!pane || pane->modal_ || !windows.insert(id).second)
                        {
                            return invalid();
                        }
                    }
                }
                else
                {
                    const bool invalid_split =
                        node.split != EDockSplit::HORIZONTAL && node.split != EDockSplit::VERTICAL;
                    const bool invalid_ratio = !std::isfinite(node.ratio) || node.ratio <= 0 || node.ratio >= 1;
                    if (invalid_split || invalid_ratio || !node.windows.empty())
                    {
                        return invalid();
                    }
                    pending.push_back(node.second);
                    pending.push_back(node.first);
                }
            }
        }
        if (prepared->order.size() != tree.nodes.size())
        {
            return invalid();
        }
        prepared->root = objectId();
        prepared->tree = std::move(tree);
        return PreparedDockTree{std::move(prepared)};
    }
    cxx::expected<void, EDockError> Root::commitDockTree(PreparedDockTree&& prepared) noexcept
    {
        if (!checkStructureSafe() || !prepared.data_ || prepared.data_->root != objectId())
        {
            return cxx::unexpected(EDockError::INVALID_DATA);
        }
        for (const auto& node : prepared.data_->tree.nodes)
        {
            for (const auto id : node.windows)
            {
                const auto* pane = findPane(id);
                if (!pane || pane->modal_)
                {
                    return cxx::unexpected(EDockError::INVALID_DATA);
                }
            }
        }
        impl_->pending_dock_tree = std::move(prepared.data_);
        ++impl_->window_revision;
        return {};
    }
    DockTree Root::captureDockTree() const
    {
        requireOwner();
        detail::ContextActivation context{impl_->context->native()};
        if (impl_->pending_dock_tree)
        {
            auto result = impl_->pending_dock_tree->tree;
            std::unordered_set<PaneId> included;
            for (const auto& node : impl_->pending_dock_tree->tree.nodes)
            {
                for (const auto& name : node.windows)
                {
                    included.insert(name);
                }
            }
            // Applying a replacement DockSpace undocks unspecified windows, without destroying
            // or hiding them. Capturing before its first draw must retain those same windows.
            for (const auto& pane_owner : impl_->panes.values())
            {
                auto* pane = pane_owner.get();

                if (pane->modal_ || included.contains(pane->id()))
                {
                    continue;
                }
                const auto* window = ImGui::FindWindowByName(pane->window_label_.c_str());
                const Rect bounds = window ? Rect{{window->Pos.x, window->Pos.y}, {window->Size.x, window->Size.y}}
                                           : Rect{{40, 40}, {640, 480}};
                const auto index = static_cast<std::uint32_t>(result.nodes.size());
                result.nodes.push_back({EDockSplit::LEAF, UINT32_MAX, UINT32_MAX, .5F, {pane->id()}});
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
        for (const auto& pane_owner : impl_->panes.values())
        {
            auto* pane = pane_owner.get();

            if (!pane || pane->modal_)
            {
                continue;
            }
            const auto* window = ImGui::FindWindowByName(pane->window_label_.c_str());
            if (!window || !window->DockNode)
            {
                const auto index = static_cast<std::uint32_t>(result.nodes.size());
                result.nodes.push_back({EDockSplit::LEAF, UINT32_MAX, UINT32_MAX, .5F, {pane->id()}});
                const Rect bounds = window ? Rect{{window->Pos.x, window->Pos.y}, {window->Size.x, window->Size.y}}
                                           : Rect{{40, 40}, {640, 480}};
                result.surfaces.push_back({index, bounds, true});
                continue;
            }
            auto* root = window->DockNode;
            while (root->ParentNode)
            {
                root = root->ParentNode;
            }
            if (!nodes.contains(root))
            {
                result.surfaces.push_back(
                    {insert(root), {{root->Pos.x, root->Pos.y}, {root->Size.x, root->Size.y}}, !root->IsDockSpace()}
                );
            }
            const auto leaf = insert(window->DockNode);
            result.nodes[leaf].windows.emplace_back(pane->id());
        }
        for (std::size_t i{}; i < pending.size(); ++i)
        {
            const auto* node = pending[i];
            if (!node->ChildNodes[0] || !node->ChildNodes[1])
            {
                continue;
            }
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

    void Root::setDockLayout(DockLayout layout)
    {
        requireOwner();
        if (!validateDockLayout(layout))
        {
            return;
        }
        impl_->split_layout = std::move(layout);
    }
    void Root::resetDockLayout(DockLayout layout)
    {
        checkContentChange();
        if (!layout.center.isValid())
        {
            if (impl_->split_layout && validateDockLayout(*impl_->split_layout))
            {
                layout = *impl_->split_layout;
            }
            else
            {
                for (const auto& pane_owner : impl_->panes.values())
                {
                    auto* pane = pane_owner.get();

                    if (!pane->modal_)
                    {
                        layout.center = pane->id();
                        break;
                    }
                }
            }
        }
        if (!validateDockLayout(layout))
        {
            return;
        }
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
        if (!valid_dimensions || !layout.center.isValid())
        {
            return lux::cxx::unexpected(EDockError::INVALID_DATA);
        }
        const std::array<PaneId, 5> ids{layout.left, layout.center, layout.right, layout.bottom, layout.toolbar};
        for (std::size_t i{}; i < ids.size(); ++i)
        {
            if (!ids[i].isValid())
            {
                continue;
            }
            const auto* pane = findPane(ids[i]);
            if (!pane || pane->modal_)
            {
                return cxx::unexpected(EDockError::INVALID_DATA);
            }
            for (std::size_t j{}; j < i; ++j)
            {
                if (ids[i] == ids[j])
                {
                    return cxx::unexpected(EDockError::INVALID_DATA);
                }
            }
        }
        return {};
    }
} // namespace lux::ui
