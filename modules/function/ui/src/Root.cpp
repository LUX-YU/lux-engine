#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Command.hpp>
#include <lux/engine/ui/detail/ContextActivation.hpp>
#include <lux/engine/ui/detail/FontValidation.hpp>
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

namespace lux::ui
{
    namespace detail
    {
        ContextActivation::ContextActivation(void* context) noexcept
        {
            previous_ = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context));
        }

        ContextActivation::~ContextActivation()
        {
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(previous_));
        }

    } // namespace detail

    namespace
    {
        [[nodiscard]] int toImGuiButton(EPointerButton button) noexcept
        {
            switch (button)
            {
            case EPointerButton::LEFT:
                return ImGuiMouseButton_Left;
            case EPointerButton::MIDDLE:
                return ImGuiMouseButton_Middle;
            case EPointerButton::RIGHT:
                return ImGuiMouseButton_Right;
            }
            return ImGuiMouseButton_Left;
        }

        [[nodiscard]] ImGuiKey toImGuiKey(EKey key) noexcept
        {
            switch (key)
            {
            case EKey::A:
                return ImGuiKey_A;
            case EKey::B:
                return ImGuiKey_B;
            case EKey::C:
                return ImGuiKey_C;
            case EKey::D:
                return ImGuiKey_D;
            case EKey::E:
                return ImGuiKey_E;
            case EKey::F:
                return ImGuiKey_F;
            case EKey::G:
                return ImGuiKey_G;
            case EKey::H:
                return ImGuiKey_H;
            case EKey::I:
                return ImGuiKey_I;
            case EKey::J:
                return ImGuiKey_J;
            case EKey::K:
                return ImGuiKey_K;
            case EKey::L:
                return ImGuiKey_L;
            case EKey::M:
                return ImGuiKey_M;
            case EKey::N:
                return ImGuiKey_N;
            case EKey::O:
                return ImGuiKey_O;
            case EKey::P:
                return ImGuiKey_P;
            case EKey::Q:
                return ImGuiKey_Q;
            case EKey::R:
                return ImGuiKey_R;
            case EKey::S:
                return ImGuiKey_S;
            case EKey::T:
                return ImGuiKey_T;
            case EKey::U:
                return ImGuiKey_U;
            case EKey::V:
                return ImGuiKey_V;
            case EKey::W:
                return ImGuiKey_W;
            case EKey::X:
                return ImGuiKey_X;
            case EKey::Y:
                return ImGuiKey_Y;
            case EKey::Z:
                return ImGuiKey_Z;
            case EKey::LEFT_SHIFT:
                return ImGuiKey_LeftShift;
            case EKey::RIGHT_SHIFT:
                return ImGuiKey_RightShift;
            case EKey::LEFT_CONTROL:
                return ImGuiKey_LeftCtrl;
            case EKey::RIGHT_CONTROL:
                return ImGuiKey_RightCtrl;
            case EKey::LEFT_ALT:
                return ImGuiKey_LeftAlt;
            case EKey::RIGHT_ALT:
                return ImGuiKey_RightAlt;
            case EKey::COUNT:
                return ImGuiKey_None;
            case EKey::NONE:
                return ImGuiKey_None;
            case EKey::TAB:
                return ImGuiKey_Tab;
            case EKey::ENTER:
                return ImGuiKey_Enter;
            case EKey::ESCAPE:
                return ImGuiKey_Escape;
            case EKey::SPACE:
                return ImGuiKey_Space;
            case EKey::BACKSPACE:
                return ImGuiKey_Backspace;
            case EKey::DELETE_KEY:
                return ImGuiKey_Delete;
            case EKey::LEFT:
                return ImGuiKey_LeftArrow;
            case EKey::RIGHT:
                return ImGuiKey_RightArrow;
            case EKey::UP:
                return ImGuiKey_UpArrow;
            case EKey::DOWN:
                return ImGuiKey_DownArrow;
            case EKey::HOME:
                return ImGuiKey_Home;
            case EKey::END:
                return ImGuiKey_End;
            }
            return ImGuiKey_None;
        }

        [[nodiscard]] ImVec4 toImGuiColor(Color value) noexcept
        {
            return ImVec4{value.red, value.green, value.blue, value.alpha};
        }

        void applyTheme(const Theme& theme) noexcept
        {
            auto& style = ImGui::GetStyle();
            style.WindowPadding = {theme.spacing.panel_padding.x, theme.spacing.panel_padding.y};
            style.FramePadding = {theme.spacing.item.x, theme.spacing.compact.y};
            style.ItemSpacing = {theme.spacing.item.x, theme.spacing.item.y};
            style.IndentSpacing = theme.metrics.tree_indent;
            style.WindowRounding = theme.metrics.rounding;
            style.ChildRounding = theme.metrics.rounding;
            style.FrameRounding = theme.metrics.rounding;
            style.PopupRounding = theme.metrics.rounding;
            style.WindowBorderSize = theme.metrics.border_width;
            style.ChildBorderSize = theme.metrics.border_width;
            style.FrameBorderSize = 0.0F;
            style.Colors[ImGuiCol_WindowBg] = toImGuiColor(theme.palette.window_background);
            style.Colors[ImGuiCol_ChildBg] = toImGuiColor(theme.palette.panel_background);
            style.Colors[ImGuiCol_FrameBg] = toImGuiColor(theme.palette.field_background);
            style.Colors[ImGuiCol_Text] = toImGuiColor(theme.palette.text);
            style.Colors[ImGuiCol_TextDisabled] = toImGuiColor(theme.palette.muted_text);
            style.Colors[ImGuiCol_Border] = toImGuiColor(theme.palette.border);
            style.Colors[ImGuiCol_CheckMark] = toImGuiColor(theme.palette.accent);
            style.Colors[ImGuiCol_SliderGrab] = toImGuiColor(theme.palette.accent);
            style.Colors[ImGuiCol_Header] = toImGuiColor(theme.palette.selection);
            style.Colors[ImGuiCol_HeaderHovered] = toImGuiColor(theme.palette.accent);
            style.Colors[ImGuiCol_HeaderActive] = toImGuiColor(theme.palette.selection);
        }

    }

    struct Root::Impl final
    {
        ~Impl();
        void queueChange(object::LuxObject&, ChangeCallback) noexcept;
        void cancelChanges(object::LuxObject&) noexcept;
        void applyPendingChanges() noexcept;
        void drawMenu(Root&) noexcept;
        void drawMenuItems(Root&, std::span<const MenuItem>) noexcept;
        void menuCommand(Root&, Command&) noexcept;
        bool shortcut(Root&, const Key&) noexcept;
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

        ImGuiContext* native{};
        std::vector<std::uint8_t> font_bytes;
        std::vector<ImWchar> font_ranges;
        Theme theme;
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
        std::array<bool, 6> modifier_keys{};
        int input_capacity{};
        ImGuiKeyChord routed_modifiers{};
        std::uint64_t sequence{};
        bool window_focused{true}, drawing{}, updating{}, docking{}, dock_layout_initialized{};
        bool input_pending{}, composing{};
        struct InputRecord final
        {
            unsigned first{}, end{}; // ImGui EventId interval; may be empty after coalescing.
            std::uint64_t sequence{};
            std::optional<ECompositionStage> composition;
        };
        std::vector<InputRecord> input_records;
        std::uint64_t accepted_input{}, adopted_input{}, focus_loss{}, cancelled_input{};
        Pane* modal{};
        std::optional<DockLayout> split_layout;
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

    Root::Impl::~Impl()
    {
        if (native)
        {
            auto* previous = ImGui::GetCurrentContext();
            if (previous == native)
            {
                previous = nullptr;
            }
            ImGui::DestroyContext(native);
            ImGui::SetCurrentContext(previous);
        }
    }

    Root::Root(object::ObjectDispatcherRef dispatcher) noexcept : LuxObject(std::move(dispatcher)) {}
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
        }
    }

    Root::CreateResult Root::create(object::ObjectDispatcherRef dispatcher, RootConfig config) noexcept
    {
        if (!dispatcher)
            return lux::cxx::unexpected(EInitError::INVALID_DISPATCHER);
        if (!dispatcher.isCurrent())
            return lux::cxx::unexpected(EInitError::WRONG_THREAD);
        {
            auto root = std::unique_ptr<Root>(new Root(std::move(dispatcher)));
            auto initialized = root->initialize(config);
            if (!initialized)
                return lux::cxx::unexpected(initialized.error());
            return root;
        }
    }

    lux::cxx::expected<void, EInitError> Root::initialize(RootConfig config) noexcept
    {
        if (!isOnAffinityThread())
            return lux::cxx::unexpected(EInitError::WRONG_THREAD);
        if (!dispatcherRef())
            return lux::cxx::unexpected(EInitError::INVALID_DISPATCHER);
        if (impl_)
            detail::failContract();
        if (config.input_capacity < 2 || config.input_capacity > std::size_t(std::numeric_limits<int>::max()))
            return lux::cxx::unexpected(EInitError::INVALID_INPUT_CAPACITY);
        const auto* font = config.font;
        {
            if (font)
            {
                if (auto valid = detail::validateFont(*font); !valid)
                {
                    return lux::cxx::unexpected(valid.error());
                }
            }
            // The partial owner releases a newly-created context on every business
            // failure, before restoring the caller's still-live context.
            struct Restore final
            {
                ImGuiContext* previous{ImGui::GetCurrentContext()};
                ~Restore()
                {
                    ImGui::SetCurrentContext(previous);
                }
            } restore;
            auto data = std::make_unique<Impl>();
            data->attachment_capacity = config.attachment_capacity;
            data->theme = config.theme;
            data->docking = config.docking;
            data->native = ImGui::CreateContext();
            data->input_capacity = static_cast<int>(config.input_capacity);
            data->input_records.reserve(config.input_capacity);
            data->native->InputEventsQueue.reserve(data->input_capacity);
            data->native->InputEventsTrail.reserve(data->input_capacity);
            ImGui::SetCurrentContext(data->native);
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.BackendRendererName = "lux.ui";
            io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
            if (config.docking)
            {
                io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
            }
            auto* atlas = ImGui::GetIO().Fonts;
            if (font)
            {
                data->font_bytes = font->bytes;
                data->font_ranges.reserve(font->ranges.size() * 2 + 1);
                for (const auto range : font->ranges)
                {
                    data->font_ranges.push_back(static_cast<ImWchar>(range.first));
                    data->font_ranges.push_back(static_cast<ImWchar>(range.last));
                }
                data->font_ranges.push_back(0);
                ImFontConfig config;
                config.FontDataOwnedByAtlas = false;
                config.FontNo = static_cast<int>(font->face);
                config.OversampleH = config.OversampleV = 1;
                atlas->TexDesiredWidth = 4096;
                if (!atlas->AddFontFromMemoryTTF(
                        data->font_bytes.data(),
                        static_cast<int>(data->font_bytes.size()),
                        font->size_pixels,
                        &config,
                        data->font_ranges.data()
                    ))
                {
                    return lux::cxx::unexpected(EInitError::ATLAS_FAILURE);
                }
            }
            if (!atlas->Build())
            {
                return lux::cxx::unexpected(EInitError::ATLAS_FAILURE);
            }
            const bool valid_extent =
                atlas->TexWidth > 0 && atlas->TexHeight > 0 && atlas->TexWidth <= 8192 && atlas->TexHeight <= 8192;
            if (!valid_extent || std::uint64_t(atlas->TexWidth) * atlas->TexHeight * 4 > 64U * 1024U * 1024U)
            {
                return lux::cxx::unexpected(EInitError::ATLAS_LIMIT);
            }
            unsigned char* pixels = nullptr;
            int width = 0;
            int height = 0;
            ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

            applyTheme(data->theme);
            impl_ = std::move(data);
            return {};
        }
    }

    void Root::requireOwner() const noexcept
    {
        if (!impl_ || !isOnAffinityThread())
            detail::failContract();
    }
    const Theme& Root::theme() const noexcept
    {
        requireOwner();
        return impl_->theme;
    }
    lux::cxx::expected<FontAtlas, EInitError> Root::fontAtlas() const noexcept
    {
        requireOwner();
        const auto* atlas = impl_->native->IO.Fonts;
        const bool valid = atlas->TexPixelsRGBA32 && atlas->TexReady && atlas->TexWidth > 0 && atlas->TexHeight > 0;
        if (!valid)
            return lux::cxx::unexpected(EInitError::ATLAS_FAILURE);
        {
            FontAtlas result;
            result.width = atlas->TexWidth;
            result.height = atlas->TexHeight;
            const auto size = std::size_t(result.width) * result.height * 4;
            const auto* pixels = reinterpret_cast<const std::uint8_t*>(atlas->TexPixelsRGBA32);
            result.pixels.assign(pixels, pixels + size);
            return result;
        }
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
        if (pane.attachedRoot() != this || !pane.visible() || !impl_->window_focused || !allowedByModal(pane))
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
        auto* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(impl_->native);
        ImGui::GetMainViewport()->PlatformHandleRaw = window ? window->nativeHandle() : nullptr;
        ImGui::SetCurrentContext(previous);
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
        const bool is_active_callback = impl_->change_batch_size != 0 || isDispatching();
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

    void Root::setMenu(std::vector<MenuItem> menu)
    {
        checkContentChange();
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
    void Root::Impl::menuCommand(Root& root, Command& command) noexcept
    {
        MenuRequest request{EMenuAction::COMMAND, menu_pane, menu_element, command};
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
            if (!item.children.empty())
            {
                if (ImGui::BeginMenu(item.label.c_str()))
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
                    ImGui::TextDisabled("%s", item.label.c_str());
                continue;
            }
            Command command{item.command.view()};
            menuCommand(root, command);
            if (ImGui::MenuItem(item.label.c_str(), item.shortcut_label.c_str(), command.checked, command.enabled))
            {
                command.phase = ECommandPhase::EXECUTE;
                menuCommand(root, command);
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
                if (ImGui::BeginMenu(item.label.c_str(), !modal))
                {
                    opened = true;
                    if (!menu_open)
                    {
                        menu_pane = focused;
                        menu_element = focused_element;
                        MenuRequest request{EMenuAction::OPEN, menu_pane, menu_element};
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
            MenuRequest request{EMenuAction::CLOSE, menu_pane, menu_element};
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
        const auto find = [&](auto&& self, std::span<const MenuItem> items) -> const MenuItem* {
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
        MenuRequest opened{EMenuAction::OPEN, menu_pane, menu_element};
        static_cast<void>(object::sendEvent(root, opened));
        Command command{item->command.view()};
        menuCommand(root, command);
        if (command.enabled)
        {
            command.phase = ECommandPhase::EXECUTE;
            menuCommand(root, command);
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
        const auto end = std::remove_if(pending, changes.end(), [&target](const Change& change) noexcept {
            return change.target == &target;
        });
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
        const bool is_active_callback = change_batch_size != 0 || Root::isDispatching();
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
            change.apply(*change.target);
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
        if (impl_->input_pending)
            return lux::cxx::unexpected(ECaptureError::INPUT_PENDING);
        const bool valid_size = std::isfinite(info.display_size.width) && info.display_size.width > 0 &&
                                std::isfinite(info.display_size.height) && info.display_size.height > 0;
        const bool valid_time = std::isfinite(info.delta_seconds) && info.delta_seconds > 0;
        const bool valid_scale = std::isfinite(info.framebuffer_scale.x) && info.framebuffer_scale.x > 0 &&
                                 std::isfinite(info.framebuffer_scale.y) && info.framebuffer_scale.y > 0;
        if (!valid_size || !valid_time || !valid_scale)
            return lux::cxx::unexpected(ECaptureError::INVALID_INPUT);
        ++impl_->layout_epoch;
        impl_->drawing = true;
        struct Finish final
        {
            Root& root;
            ~Finish()
            {
                root.impl_->drawing = false;
            }
        } finish{*this};
        detail::ContextActivation active{impl_->native};
        auto& io = ImGui::GetIO();
        io.DisplaySize = {info.display_size.width, info.display_size.height};
        io.DeltaTime = info.delta_seconds;
        io.DisplayFramebufferScale = {info.framebuffer_scale.x, info.framebuffer_scale.y};
        applyTheme(impl_->theme);
        impl_->draw_focused = impl_->draw_hovered = nullptr;
        impl_->draw_focused_element = impl_->draw_hovered_element = nullptr;
        ImGui::NewFrame();
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
        ImGui::Render();
        ++impl_->sequence;
        impl_->input_pending = !impl_->native->InputEventsTrail.empty() || !impl_->input_records.empty();
        const auto captured = output.captureCurrent();
        if (captured != ECaptureError::NONE)
            return lux::cxx::unexpected(captured);
        return {};
    }

    PreparedAttachment::PreparedAttachment(std::unique_ptr<detail::AttachmentState> state) noexcept
        : state_(std::move(state))
    {}
    PreparedAttachment::~PreparedAttachment() noexcept
    {
        if (!state_)
            return;
        if (state_->root)
            state_->root->abandonAttachment(*state_);
        else if (state_->pane && state_->pane->preparation_ == state_.get())
            state_->pane->preparation_ = nullptr;
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
        if (state.pane && state.pane->preparation_ == &state)
            state.pane->preparation_ = nullptr;
        state.root = nullptr;
        state.pane = nullptr;
        state.valid = false;
    }
    bool Root::attachmentSafe() const noexcept
    {
        return isOnAffinityThread() && !impl_->drawing && !impl_->updating && !impl_->layout_depth &&
               !impl_->active_change && !isDispatching();
    }
    Root::AttachmentResult Root::prepareMount(Pane& pane)
    {
        return prepareAttachment(pane, true);
    }
    Root::AttachmentResult Root::prepareDetach(Pane& pane)
    {
        return prepareAttachment(pane, false);
    }
    Root::AttachmentResult Root::prepareAttachment(Pane& pane, bool mount)
    {
        if (!isOnAffinityThread() || !pane.isOnAffinityThread())
            return lux::cxx::unexpected(EAttachmentError::WRONG_THREAD);
        if (!attachmentSafe() || impl_->preparation || pane.preparation_)
            return lux::cxx::unexpected(EAttachmentError::BUSY);
        if (pane.dispatcherRef() != dispatcherRef())
            return lux::cxx::unexpected(EAttachmentError::WRONG_DISPATCHER);
        if (mount && (pane.attachedRoot() || pane.parent()))
            return lux::cxx::unexpected(EAttachmentError::ALREADY_ATTACHED);
        if (!mount && (pane.attachedRoot() != this || pane.parent() != this))
            return lux::cxx::unexpected(EAttachmentError::NOT_ATTACHED);
        auto prepared = std::make_unique<detail::AttachmentState>();
        std::size_t windows{};
        bool invalid{};
        const auto visit = [&](auto&& self, object::LuxObject& node) -> void {
            if (auto* window = dynamic_cast<Pane*>(&node))
            {
                const bool wrong_root = mount ? window->attachedRoot() != nullptr : window->attachedRoot() != this;
                invalid |= wrong_root;
                prepared->nodes.push_back({window, nullptr});
                ++windows;
            }
            else if (auto* element = dynamic_cast<Element*>(&node))
            {
                invalid |= !element->containingPane();
                prepared->nodes.push_back({nullptr, element});
            }
            else
            {
                invalid = true;
                return;
            }
            for (auto* child = node.firstChild(); child; child = child->nextSibling())
                self(self, *child);
        };
        visit(visit, pane);
        if (invalid)
            return lux::cxx::unexpected(EAttachmentError::INVALID_TREE);
        if (mount)
        {
            std::vector<PaneIdView> names;
            names.reserve(windows);
            for (auto node : prepared->nodes)
                if (node.pane)
                {
                    auto name = node.pane->id().view();
                    if (findPane(name) || std::ranges::find(names, name) != names.end())
                        return lux::cxx::unexpected(EAttachmentError::DUPLICATE_ID);
                    names.push_back(name);
                }
            const auto active =
                std::ranges::count_if(impl_->registrations, [](const auto& entry) { return entry.object; });
            if (prepared->nodes.size() >
                impl_->attachment_capacity - std::min<std::size_t>(active, impl_->attachment_capacity))
                return lux::cxx::unexpected(EAttachmentError::CAPACITY);
            impl_->compactRegistrations();
            impl_->registrations.reserve(impl_->registrations.size() + prepared->nodes.size());
            impl_->windows.reserve(impl_->windows.size() + windows);
        }
        prepared->root = this;
        prepared->pane = &pane;
        prepared->revision = impl_->structure_revision;
        prepared->mount = mount;
        impl_->preparation = prepared.get();
        pane.preparation_ = prepared.get();
        return PreparedAttachment{std::move(prepared)};
    }
    lux::cxx::expected<AttachmentCommit, EAttachmentError> Root::commit(PreparedAttachment& token) noexcept
    {
        if (!isOnAffinityThread())
            return lux::cxx::unexpected(EAttachmentError::WRONG_THREAD);
        if (!attachmentSafe())
            return lux::cxx::unexpected(EAttachmentError::BUSY);
        auto* state = token.state_.get();
        const bool stale = !state || state->root != this || !state->pane || !state->valid ||
                           state->revision != impl_->structure_revision;
        if (stale)
            return lux::cxx::unexpected(EAttachmentError::STALE_PREPARATION);
        // Consume the public token before notifications. Callers may release or replace it in a callback.
        auto committed = std::move(token.state_);
        auto& pane = *state->pane;
        const bool mount = state->mount;
        // Clear preparation before changing links. No callbacks or allocations until every link is adopted.
        abandonAttachment(*state);
        if (mount)
        {
            pane.attachTo(*this);
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
            pane.detachFromParent();
            for (auto node : state->nodes)
                if (node.pane)
                    node.pane->root_ = nullptr;
        }
        AttachmentCommit result{mount, {}};
        const auto append = [&](object::SignalDelivery delivered) {
            result.notifications.direct += delivered.direct;
            result.notifications.queued += delivered.queued;
            result.notifications.full += delivered.full;
            result.notifications.closed += delivered.closed;
        };
        // A notification cannot mutate/destroy this subtree, even after it has left Root's routing chain.
        pane.beginTreeVisit();
        if (!mount)
            for (auto node : state->nodes)
                append(emit(objectRemoved, node.pane ? static_cast<object::LuxObject*>(node.pane) : node.element));
        append(emit(attachmentChanged, AttachmentChanged{pane.id(), mount}));
        pane.endTreeVisit();
        return result;
    }

    void Root::registerPane(Pane& pane)
    {
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
        // The rooted destructor adapter keeps its established notification order until P12.
        if (notify)
            static_cast<void>(emit(objectRemoved, static_cast<object::LuxObject*>(&pane)));
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
    }

    void Root::unregisterElement(Element& element, bool notify) noexcept
    {
        checkDestruction(element);
        checkContentChange();
        // The rooted destructor adapter keeps its established notification order until P12.
        if (notify)
            static_cast<void>(emit(objectRemoved, static_cast<object::LuxObject*>(&element)));
        ++impl_->structure_revision;
        if (impl_->menu_element == &element)
            impl_->menu_element = nullptr;
        impl_->cancelChanges(element);
        impl_->registrations[element.registration_slot_] = {};
        impl_->registration_holes = true;
        if (impl_->focused_element == &element)
        {
            detail::ContextActivation context{impl_->native};
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
        const bool is_frozen_visit = impl_->drawing || impl_->layout_depth != 0 || isDispatching();
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
        if (!impl_->input_pending && impl_->focus_loss > impl_->cancelled_input)
        {
            // Loss ends the current interaction even while presentation has no writable frame.
            // Its older native batch is still consumed by ImGui, but cannot replay business commands.
            impl_->cancelled_input = impl_->focus_loss;
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
        if (std::exchange(impl_->input_pending, false))
        {
            detail::ContextActivation context{impl_->native};
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
            entry.update(entry.object);
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
        if (element.attachedRoot() != this || !element.displayed() || !element.enabled() || !impl_->window_focused ||
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
        detail::ContextActivation context{impl_->native};
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
        detail::ContextActivation context{impl_->native};
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
        detail::ContextActivation context{impl_->native};
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
            const auto visible = [&](const std::string& id) {
                const auto* pane = findPane(PaneIdView{id});
                return pane && pane->visible();
            };
            const float left =
                size.x >= 700 && visible(layout.left) ? std::clamp(layout.left_width, 160.0F, size.x * 0.3F) : 0;
            const float right =
                size.x >= 1000 && visible(layout.right) ? std::clamp(layout.right_width, 220.0F, size.x * 0.35F) : 0;
            const float bottom =
                size.y >= 450 && visible(layout.bottom) ? std::clamp(layout.bottom_height, 100.0F, size.y * 0.4F) : 0;
            const auto splitter = [&](const char* id, ImVec2 pos, ImVec2 extent, bool vertical, float& value) {
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
                for (int i = 0; i < impl_->native->OpenPopupStack.Size; ++i)
                    if (auto* window = impl_->native->OpenPopupStack[i].Window;
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
        detail::ContextActivation context{impl_->native};
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
        detail::ContextActivation context{impl_->native};
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
        impl_->window_focused = focused_value;
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
        const auto route = [&](const ImGuiInputEvent& input) noexcept {
            // An earlier handler can acquire, transfer or release capture. Resolve
            // the current owner for each event, never cache it for the whole batch.
            auto capture = impl_->pointer_capture;
            if (input.Type == ImGuiInputEventType_Focus)
            {
                deliverWindowFocus(input.AppFocused.Focused);
                if (!input.AppFocused.Focused)
                    impl_->cancelled_input = impl_->adopted_input;
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
                for (std::size_t i = 1; i < static_cast<std::size_t>(EKey::COUNT); ++i)
                    if (toImGuiKey(static_cast<EKey>(i)) == input.Key.Key)
                        value = Key{static_cast<EKey>(i), input.Key.Down};
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
        std::size_t complete{};
        int trail{};
        for (auto& record : impl_->input_records)
        {
            if (record.first != record.end)
            {
                while (trail < impl_->native->InputEventsTrail.Size)
                {
                    const auto& input = impl_->native->InputEventsTrail[trail];
                    if (input.EventId >= record.end)
                        break;
                    ++trail;
                    if (input.EventId < record.first)
                        continue;
                    impl_->adopted_input = record.sequence;
                    if (record.sequence > impl_->cancelled_input)
                        route(input);
                    record.first = input.EventId + 1;
                }
                if (record.first != record.end)
                    break; // ImGui trickles the remainder.
            }
            impl_->adopted_input = record.sequence;
            if (record.composition && record.sequence > impl_->cancelled_input)
            {
                const auto stage = *record.composition;
                impl_->composing = stage == ECompositionStage::STARTED || stage == ECompositionStage::UPDATED;
                Impl::Target target{impl_->focused_element};
                if (!target)
                    target = focusedPane();
                if (target && allowedByModal(*target.pane()))
                {
                    VInputEvent event = Composition{stage};
                    static_cast<void>(object::sendEvent(*target, event));
                }
            }
            ++complete;
        }
        impl_->input_records.erase(impl_->input_records.begin(), impl_->input_records.begin() + complete);
        if (!ImGui::IsAnyMouseDown())
            impl_->pointer_capture = {};
    }

    lux::cxx::expected<void, EInputError> Root::feedInput(const VInputEvent& event, std::uint64_t sequence) noexcept
    {
        requireOwner();
        detail::ContextActivation context{impl_->native};
        auto& io = ImGui::GetIO();
        if (!io.AppAcceptingEvents)
            return lux::cxx::unexpected(EInputError::CLOSED);
        const bool valid = std::visit(
            [](const auto& value) noexcept {
                using Value = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<Value, PointerMove>)
                    return std::isfinite(value.position.x) && std::isfinite(value.position.y);
                else if constexpr (std::same_as<Value, PointerWheel>)
                    return std::isfinite(value.delta.x) && std::isfinite(value.delta.y);
                else if constexpr (std::same_as<Value, PointerButton>)
                    return value.button >= EPointerButton::LEFT && value.button <= EPointerButton::RIGHT;
                else if constexpr (std::same_as<Value, Key>)
                    return value.key > EKey::NONE && value.key < EKey::COUNT;
                else if constexpr (std::same_as<Value, Text>)
                    return value.codepoint > 0 && value.codepoint <= 0x10FFFF &&
                           (value.codepoint < 0xD800 || value.codepoint > 0xDFFF);
                else if constexpr (std::same_as<Value, Composition>)
                    return value.stage >= ECompositionStage::STARTED && value.stage <= ECompositionStage::CANCELLED;
                else
                    return !std::same_as<Value, PointerCancel>;
            },
            event
        );
        const bool invalid_sequence = sequence && sequence <= impl_->accepted_input;
        if (!valid || invalid_sequence || impl_->accepted_input == UINT64_MAX)
            return lux::cxx::unexpected(EInputError::INVALID_INPUT);
        const auto* key_event = std::get_if<Key>(&event);
        const bool modifier = key_event && key_event->key >= EKey::LEFT_SHIFT;
        const int required = modifier ? 2 : 1;
        // Preflight the entire native event, including its aggregate modifier.
        // The existing ImGui queue is the only pending input store.
        if (impl_->native->InputEventsQueue.Size > impl_->input_capacity - required ||
            impl_->input_records.size() == std::size_t(impl_->input_capacity))
            return lux::cxx::unexpected(EInputError::FULL);
        const auto first = impl_->native->InputEventsNextEventId;
        std::visit(
            [&](const auto& value) {
                using Value = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<Value, PointerMove>)
                {
                    io.AddMousePosEvent(value.position.x, value.position.y);
                }
                else if constexpr (std::same_as<Value, PointerButton>)
                {
                    io.AddMouseButtonEvent(toImGuiButton(value.button), value.down);
                }
                else if constexpr (std::same_as<Value, PointerWheel>)
                {
                    io.AddMouseWheelEvent(value.delta.x, value.delta.y);
                }
                else if constexpr (std::same_as<Value, Key>)
                {
                    constexpr std::array physical_modifiers{
                        EKey::LEFT_CONTROL,
                        EKey::RIGHT_CONTROL,
                        EKey::LEFT_SHIFT,
                        EKey::RIGHT_SHIFT,
                        EKey::LEFT_ALT,
                        EKey::RIGHT_ALT
                    };
                    constexpr std::array aggregate_modifiers{ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt};
                    for (std::size_t index = 0; index < physical_modifiers.size(); ++index)
                    {
                        if (value.key == physical_modifiers[index])
                        {
                            // ImGui does not derive aggregate modifiers from
                            // side-specific events. Preserve the other side.
                            impl_->modifier_keys[index] = value.down;
                            const auto pair = index / 2;
                            io.AddKeyEvent(
                                aggregate_modifiers[pair],
                                impl_->modifier_keys[pair * 2] || impl_->modifier_keys[pair * 2 + 1]
                            );
                            break;
                        }
                    }
                    const auto key = toImGuiKey(value.key);
                    if (key != ImGuiKey_None)
                    {
                        io.AddKeyEvent(key, value.down);
                    }
                }
                else if constexpr (std::same_as<Value, Text>)
                {
                    io.AddInputCharacter(static_cast<unsigned int>(value.codepoint));
                }
                else if constexpr (std::same_as<Value, WindowFocus>)
                {
                    impl_->window_focused = value.focused;
                    if (!value.focused)
                    {
                        impl_->modifier_keys.fill(false);
                    }
                    io.AddFocusEvent(value.focused);
                }
            },
            event
        );
        const auto accepted = sequence ? sequence : impl_->accepted_input + 1;
        if (const auto* focus = std::get_if<WindowFocus>(&event); focus && !focus->focused)
            impl_->focus_loss = accepted;
        const auto* composition = std::get_if<Composition>(&event);
        impl_->input_records.push_back(
            {first,
             impl_->native->InputEventsNextEventId,
             accepted,
             composition ? std::optional{composition->stage} : std::nullopt}
        );
        impl_->accepted_input = accepted;
        return {};
    }

    void Root::closeInput() noexcept
    {
        if (!impl_)
            return; // No input was admitted if initialization never completed.
        requireOwner();
        if (!impl_->native->IO.AppAcceptingEvents)
            return;
        impl_->native->IO.SetAppAcceptingEvents(false);
        impl_->cancelled_input = impl_->accepted_input;
        deliverWindowFocus(false);
    }

    InputSnapshot Root::inputSnapshot() const noexcept
    {
        requireOwner();
        detail::ContextActivation context{impl_->native};
        InputSnapshot result;
        const auto& io = ImGui::GetIO();
        for (std::size_t index = 1; index < result.held.size(); ++index)
        {
            const auto key = toImGuiKey(static_cast<EKey>(index));
            result.held[index] = ImGui::IsKeyDown(key);
            result.pressed[index] = ImGui::IsKeyPressed(key, false);
        }
        for (std::size_t index = 0; index < result.buttons.size(); ++index)
        {
            result.buttons[index] = ImGui::IsMouseDown(toImGuiButton(static_cast<EPointerButton>(index)));
        }
        result.pointer_delta = {io.MouseDelta.x, io.MouseDelta.y};
        result.wheel = {io.MouseWheelH, io.MouseWheel};
        result.window_focused = !io.AppFocusLost;
        result.composing = impl_->composing;
        result.sequence = impl_->adopted_input;
        result.modal_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
        result.keyboard_blocked = impl_->composing || io.WantTextInput || ImGui::IsAnyItemActive() || result.modal_open;
        return result;
    }

    DockState Root::captureDockState() const
    {
        requireOwner();
        detail::ContextActivation context{impl_->native};
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
        detail::ContextActivation context{impl_->native};
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
