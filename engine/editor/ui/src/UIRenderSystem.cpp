#include <lux/engine/editor/ui/UIRenderSystem.hpp>
#include <lux/engine/scene/SceneBuilder.hpp>
#include <lux/engine/ui/detail/PaneStateAccess.hpp>
#include <lux/engine/ui/rendering/UiRenderFeature.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <limits>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <lux/engine/ui/detail/ContextAccess.hpp>
#include <lux/engine/ui/detail/ContextActivation.hpp>
#include <lux/engine/ui/detail/UiContract.hpp>

namespace lux::editor::ui
{
using namespace ::lux::ui;
namespace detail
{
struct PaneControl final
{
    explicit PaneControl(UIRenderSystem *value) noexcept
        : session(value), owner(std::this_thread::get_id()), owner_token(::lux::ui::detail::currentUiThreadToken())
    {
    }

    UIRenderSystem *session{nullptr};
    const std::thread::id owner;
    const void *const owner_token;
};

} // namespace detail

namespace
{
using ScopedImGuiContext = ::lux::ui::detail::ContextActivation;

struct PaneRecord final
{
    std::uint64_t token{0};
    PaneId id;
    lux::object::ObjectWeakRef lifetime;
    bool tombstone{false};
};

struct PaneHandle final
{
    std::uint64_t token{0};
    lux::object::ObjectWeakRef lifetime;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return token != 0;
    }

    void reset() noexcept
    {
        token = 0;
        lifetime = {};
    }
};

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

} // namespace

struct UIRenderSystem::Impl final
{
    Impl(UIRenderSystem *value, Context cpu, Theme theme_value, lux::object::ObjectDispatcherRef dispatcher)
        : owner(value), context(std::move(cpu)), theme(std::move(theme_value)), messages(std::move(dispatcher))
    {
    }

    [[nodiscard]] lux::object::ObjectDispatcherRef dispatcherRef() const noexcept
    {
        return messages;
    }

    UIRenderSystem *owner{nullptr};
    Context context;
    UiTextInputAnchor backend_anchor, captured_anchor;
    std::uint64_t frame_sequence{};
    bool window_focused{true};
    std::array<bool, 6> modifier_keys{};
    Theme theme;
    lux::object::ObjectDispatcherRef messages;
    CommandRouter command_router;
    std::function<EditorResult<bool>(std::filesystem::path &)> select_existing_file;
    std::array<std::shared_ptr<UiRenderFrame>, render::RenderProgramChannel<>::request_slot_count + 1> frames;
    std::size_t current_frame{frames.size()};
    render::RenderProgram<> packet;
    enum class CaptureState
    {
        EMPTY,
        BUILDING,
        CAPTURED,
        PACKED
    };
    CaptureState capture_state{CaptureState::EMPTY};
    bool frames_stopped{}, clear_pending{};
    std::uint64_t captures{};
    render::UiRenderOperationIds operations;
    scene::SceneStageResult publication{scene::ESceneProgress::COMPLETE};
    std::vector<PaneRecord> panes;
    std::vector<PaneRecord> pending_panes;
    std::vector<UiContextId> focused_local_context_ids;
    std::vector<UiContextId> focused_context_ids;
    std::vector<UiContextIdView> focused_contexts;
    std::vector<UiContextIdView> frame_context_scratch;
    std::vector<UiContextIdView> frame_focused_contexts;
    PaneHandle focused_pane;
    PaneHandle hovered_pane;
    PaneHandle pending_focus;
    std::uint64_t next_token{1};
    std::optional<SplitLayout> split_layout;
    bool docking{};
    bool dock_layout_initialized{};
    std::array<ImGuiID, 5> dock_regions{};

    [[nodiscard]] static PaneHandle handle(const PaneRecord &record)
    {
        return {record.token, record.lifetime};
    }

    [[nodiscard]] static Pane *resolve(const PaneHandle &pane) noexcept
    {
        return pane ? pane.lifetime.getAsOnCurrent<Pane>() : nullptr;
    }

    [[nodiscard]] static Pane *resolve(PaneRecord &record) noexcept
    {
        if (record.tombstone)
        {
            return nullptr;
        }
        auto *pane = record.lifetime.getAsOnCurrent<Pane>();
        if (!pane)
        {
            record.tombstone = true;
        }
        return pane;
    }

    [[nodiscard]] PaneRecord *findPane(std::uint64_t token) noexcept
    {
        const auto active = std::ranges::find(panes, token, &PaneRecord::token);
        if (active != panes.end())
        {
            return std::addressof(*active);
        }
        const auto pending = std::ranges::find(pending_panes, token, &PaneRecord::token);
        return pending == pending_panes.end() ? nullptr : std::addressof(*pending);
    }

    [[nodiscard]] const PaneRecord *findPane(std::uint64_t token) const noexcept
    {
        const auto active = std::ranges::find(panes, token, &PaneRecord::token);
        if (active != panes.end())
        {
            return std::addressof(*active);
        }
        const auto pending = std::ranges::find(pending_panes, token, &PaneRecord::token);
        return pending == pending_panes.end() ? nullptr : std::addressof(*pending);
    }

    [[nodiscard]] Pane *resolveRegistered(const PaneHandle &pane) noexcept
    {
        auto *record = findPane(pane.token);
        return record ? resolve(*record) : nullptr;
    }

    [[nodiscard]] Pane *resolveRegistered(const PaneHandle &pane) const noexcept
    {
        const auto *record = findPane(pane.token);
        return record && !record->tombstone ? pane.lifetime.getAsOnCurrent<Pane>() : nullptr;
    }

    void compactPaneRecords()
    {
        const auto dead = [](const PaneRecord &record) { return record.tombstone || record.lifetime.expired(); };
        std::erase_if(panes, dead);
        std::erase_if(pending_panes, dead);
    }

    void publishPendingPanes()
    {
        compactPaneRecords();
        for (auto &record : pending_panes)
        {
            if (!record.tombstone && record.lifetime.alive())
            {
                panes.push_back(std::move(record));
            }
        }
        pending_panes.clear();
    }

    [[nodiscard]] static bool sameContexts(const std::vector<UiContextId> &owned,
                                           std::span<const UiContextIdView> views) noexcept
    {
        if (owned.size() != views.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < owned.size(); ++index)
        {
            if (owned[index].view() != views[index])
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static std::vector<UiContextId> ownContexts(std::span<const UiContextIdView> contexts)
    {
        std::vector<UiContextId> result;
        result.reserve(contexts.size());
        for (const auto context : contexts)
        {
            if (context.isValid())
            {
                result.emplace_back(context.name());
            }
        }
        return result;
    }

    void rebuildFocusedContexts()
    {
        auto *focused = resolveRegistered(focused_pane);
        if (!focused)
        {
            focused_pane.reset();
        }

        focused_context_ids.clear();
        focused_contexts.clear();
        const auto append_unique = [&](UiContextIdView context) {
            if (!context.isValid())
            {
                return;
            }
            const auto found = std::ranges::find_if(
                focused_context_ids, [context](const UiContextId &value) { return value.view() == context; });
            if (found == focused_context_ids.end())
            {
                focused_context_ids.emplace_back(context.name());
            }
        };

        for (auto iterator = focused_local_context_ids.rbegin(); iterator != focused_local_context_ids.rend();
             ++iterator)
        {
            append_unique(iterator->view());
        }
        if (focused)
        {
            for (const auto context : focused->contexts())
            {
                append_unique(context);
            }
        }
        append_unique(kGlobalContext);

        focused_contexts.reserve(focused_context_ids.size());
        for (const auto &context : focused_context_ids)
        {
            focused_contexts.push_back(context.view());
        }
        owner->updateCommandRoute(focused, focused_contexts);
    }

    void clearFocus()
    {
        if (!focused_pane)
        {
            return;
        }
        auto previous = std::move(focused_pane);
        focused_pane.reset();
        focused_local_context_ids.clear();
        rebuildFocusedContexts();
        if (auto *pane = resolve(previous))
        {
            ::lux::ui::detail::PaneStateAccess::setFocused(*pane, false);
        }
    }

    void commitFocus(PaneHandle pane, std::span<const UiContextIdView> local_contexts)
    {
        if (pane && !resolveRegistered(pane))
        {
            pane.reset();
        }
        const bool pane_changed = focused_pane.token != pane.token;
        const bool contexts_changed = !sameContexts(focused_local_context_ids, local_contexts);
        if (!pane_changed && !contexts_changed)
        {
            return;
        }

        auto owned_contexts = ownContexts(local_contexts);
        auto previous = focused_pane;
        focused_pane = pane;
        focused_local_context_ids = std::move(owned_contexts);
        rebuildFocusedContexts();

        if (pane_changed)
        {
            if (auto *old_pane = resolve(previous))
            {
                ::lux::ui::detail::PaneStateAccess::setFocused(*old_pane, false);
            }

            if (focused_pane.token != pane.token)
            {
                return;
            }
            auto *new_pane = resolveRegistered(focused_pane);
            if (!new_pane)
            {
                clearFocus();
                return;
            }
            ::lux::ui::detail::PaneStateAccess::setFocused(*new_pane, true);
        }
    }

    void commitHover(PaneHandle pane)
    {
        if (pane && !resolveRegistered(pane))
        {
            pane.reset();
        }
        if (hovered_pane.token == pane.token)
        {
            return;
        }
        if (auto *previous = resolve(hovered_pane))
        {
            ::lux::ui::detail::PaneStateAccess::setHovered(*previous, false);
        }
        hovered_pane = pane;
        if (auto *current = resolveRegistered(hovered_pane))
        {
            ::lux::ui::detail::PaneStateAccess::setHovered(*current, true);
        }
    }
};

PaneRegistration::PaneRegistration(std::weak_ptr<detail::PaneControl> control, std::uint64_t token) noexcept
    : control_(std::move(control)), token_(token)
{
}

PaneRegistration::PaneRegistration(PaneRegistration &&other) noexcept
    : control_(std::move(other.control_)), token_(std::exchange(other.token_, 0))
{
}

PaneRegistration &PaneRegistration::operator=(PaneRegistration &&other) noexcept
{
    if (this != std::addressof(other))
    {
        reset();
        control_ = std::move(other.control_);
        token_ = std::exchange(other.token_, 0);
    }
    return *this;
}

PaneRegistration::~PaneRegistration()
{
    reset();
}

void PaneRegistration::reset() noexcept
{
    if (token_ == 0)
    {
        return;
    }
    if (const auto control = control_.lock())
    {
        ::lux::ui::detail::requireUiOwner(control->owner, control->owner_token);
        if (control->session)
        {
            control->session->unregisterPane(token_);
        }
    }
    token_ = 0;
    control_.reset();
}

UIRenderSystem::UIRenderSystem(system::SystemInstanceId id, scene::SceneInstanceId scene,
                               render::RenderRuntime &runtime, simulation::ecs::Registry &registry,
                               render::RenderSceneLease lease, Context context, const UIRenderSystemConfig &info,
                               object::ObjectDispatcherRef dispatcher)
    : object::Object<UIRenderSystem, RenderSystem>(id, scene, runtime, registry, std::move(lease), 1024.0, {}, {},
                                                   nullptr, dispatcher),
      impl_(std::make_unique<Impl>(this, std::move(context), info.theme, std::move(dispatcher))),
      control_(std::make_shared<detail::PaneControl>(this))
{
    impl_->docking = info.docking;
    impl_->select_existing_file = info.select_existing_file;
    impl_->operations = runtime.features().ops<render::UiRenderOperationIds>("UiRender");
    for (auto &slot : impl_->frames)
    {
        slot = std::make_shared<UiRenderFrame>();
    }
    initialize();
}

void UIRenderSystem::initialize()
{
    ScopedImGuiContext context{::lux::ui::detail::ContextAccess::native(impl_->context)};
    auto &platform = ImGui::GetPlatformIO();
    platform.Platform_ImeUserData = impl_.get();
    platform.Platform_SetImeDataFn = [](ImGuiContext *, ImGuiViewport *viewport, ImGuiPlatformImeData *data) {
        auto &state = *static_cast<Impl *>(ImGui::GetPlatformIO().Platform_ImeUserData);
        state.backend_anchor = {};
        if (!viewport || viewport != ImGui::GetMainViewport())
        {
            return;
        }
        state.backend_anchor.caret = {data->InputPos.x - viewport->Pos.x, data->InputPos.y - viewport->Pos.y};
        state.backend_anchor.line_height = data->InputLineHeight;
        state.backend_anchor.want_visible = data->WantVisible;
        state.backend_anchor.valid = data->WantVisible;
    };
    impl_->focused_contexts.reserve(8);
    impl_->focused_local_context_ids.reserve(8);
    impl_->focused_context_ids.reserve(8);
    impl_->frame_context_scratch.reserve(8);
    impl_->frame_focused_contexts.reserve(8);
    impl_->rebuildFocusedContexts();
}

void UIRenderSystem::openAsset(asset::AssetId source)
{
    notify<assetOpenRequested>(source);
}

EditorResult<bool> UIRenderSystem::selectExistingFile(std::filesystem::path &path)
{
    if (!impl_->select_existing_file)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "ui.file-dialog"});
    }
    return impl_->select_existing_file(path);
}

UIRenderSystem::~UIRenderSystem() noexcept
{
    ::lux::ui::detail::requireUiOwner(control_->owner, control_->owner_token);
    control_->session = nullptr;
    impl_->clearFocus();
    impl_->commitHover({});
}

void UIRenderSystem::updateCommandRoute(lux::object::LuxObject *activation_scope,
                                        std::span<const UiContextIdView> contexts)
{
    impl_->command_router.updateRoute(activation_scope, contexts);
}

lux::cxx::expected<PaneRegistration, EUiRegistrationError> UIRenderSystem::registerPane(Pane &pane)
{
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    if (!pane.id().isValid())
    {
        return lux::cxx::unexpected<EUiRegistrationError>{EUiRegistrationError::INVALID_ID};
    }
    if (!impl_->context.frameOpen())
    {
        impl_->compactPaneRecords();
    }
    const auto duplicate_id = [&](const PaneRecord &record) {
        return !record.tombstone && record.lifetime.alive() && record.id.view() == pane.id().view();
    };
    if (std::ranges::any_of(impl_->panes, duplicate_id) || std::ranges::any_of(impl_->pending_panes, duplicate_id))
    {
        return lux::cxx::unexpected<EUiRegistrationError>{EUiRegistrationError::DUPLICATE_PANE_ID};
    }
    if (pane.dispatcherRef() != impl_->dispatcherRef())
    {
        return lux::cxx::unexpected<EUiRegistrationError>{EUiRegistrationError::FOREIGN_DISPATCHER};
    }
    const auto token = impl_->next_token++;
    PaneRecord record{token, pane.id(), pane.weakRef(), false};
    if (impl_->context.frameOpen())
    {
        impl_->pending_panes.push_back(std::move(record));
    }
    else
    {
        impl_->panes.push_back(std::move(record));
    }
    return PaneRegistration{control_, token};
}

CommandRouter &UIRenderSystem::commandRouter() noexcept
{
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    return impl_->command_router;
}

const CommandRouter &UIRenderSystem::commandRouter() const noexcept
{
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    return impl_->command_router;
}

lux::object::ObjectDispatcherRef UIRenderSystem::dispatcherRef() const noexcept
{
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    return impl_->dispatcherRef();
}

bool UIRenderSystem::requestFocus(PaneIdView pane)
{
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    if (!impl_->context.frameOpen())
    {
        impl_->compactPaneRecords();
    }
    const auto found = std::ranges::find_if(impl_->panes, [&](const PaneRecord &record) {
        return !record.tombstone && record.id.view() == pane && record.lifetime.alive();
    });
    if (found == impl_->panes.end())
    {
        return false;
    }
    auto *resolved = found->lifetime.getAsOnCurrent<Pane>();
    if (!resolved || !resolved->visible())
    {
        return false;
    }
    impl_->pending_focus = Impl::handle(*found);
    return true;
}

Pane *UIRenderSystem::focusedPane() const noexcept
{
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    return impl_->resolveRegistered(impl_->focused_pane);
}

std::span<const UiContextIdView> UIRenderSystem::focusedContexts() const noexcept
{
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    return impl_->focused_contexts;
}

void UIRenderSystem::feedInput(const UiInputEvent &event)
{
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    ScopedImGuiContext context{::lux::ui::detail::ContextAccess::native(impl_->context)};
    auto &io = ImGui::GetIO();
    std::visit(
        [&](const auto &value) {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<Value, UiPointerMove>)
            {
                io.AddMousePosEvent(value.position.x, value.position.y);
            }
            else if constexpr (std::same_as<Value, UiPointerButton>)
            {
                io.AddMouseButtonEvent(toImGuiButton(value.button), value.down);
            }
            else if constexpr (std::same_as<Value, UiPointerWheel>)
            {
                io.AddMouseWheelEvent(value.delta.x, value.delta.y);
            }
            else if constexpr (std::same_as<Value, UiKey>)
            {
                constexpr std::array physical_modifiers{EKey::LEFT_CONTROL, EKey::RIGHT_CONTROL, EKey::LEFT_SHIFT,
                                                        EKey::RIGHT_SHIFT,  EKey::LEFT_ALT,      EKey::RIGHT_ALT};
                constexpr std::array aggregate_modifiers{ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt};
                for (std::size_t index = 0; index < physical_modifiers.size(); ++index)
                {
                    if (value.key == physical_modifiers[index])
                    {
                        // ImGui does not derive aggregate modifiers from
                        // side-specific events. Preserve the other side.
                        impl_->modifier_keys[index] = value.down;
                        const auto pair = index / 2;
                        io.AddKeyEvent(aggregate_modifiers[pair],
                                       impl_->modifier_keys[pair * 2] || impl_->modifier_keys[pair * 2 + 1]);
                        break;
                    }
                }
                const auto key = toImGuiKey(value.key);
                if (key != ImGuiKey_None)
                {
                    io.AddKeyEvent(key, value.down);
                }
            }
            else if constexpr (std::same_as<Value, UiText>)
            {
                io.AddInputCharacter(static_cast<unsigned int>(value.codepoint));
            }
            else if constexpr (std::same_as<Value, UiWindowFocus>)
            {
                impl_->window_focused = value.focused;
                impl_->captured_anchor = {};
                if (!value.focused)
                {
                    impl_->modifier_keys.fill(false);
                }
                io.AddFocusEvent(value.focused);
            }
        },
        event);
}

Frame UIRenderSystem::beginFrame(FrameInfo info)
{
    if (!canBuildFrame())
    {
        ::lux::ui::detail::failUiContract();
    }
    const auto free = std::ranges::find_if(impl_->frames, [](const auto &slot) { return slot.use_count() == 1; });
    impl_->current_frame = static_cast<std::size_t>(free - impl_->frames.begin());
    impl_->capture_state = Impl::CaptureState::BUILDING;
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    if (impl_->context.frameOpen())
    {
        ::lux::ui::detail::failUiContract();
    }
    impl_->captured_anchor = {};
    if (impl_->frame_sequence != (std::numeric_limits<std::uint64_t>::max)())
    {
        ++impl_->frame_sequence;
    }
    impl_->publishPendingPanes();
    impl_->compactPaneRecords();
    if (impl_->focused_pane && !impl_->resolveRegistered(impl_->focused_pane))
    {
        impl_->clearFocus();
    }
    if (impl_->hovered_pane && !impl_->resolveRegistered(impl_->hovered_pane))
    {
        impl_->commitHover({});
    }
    return Frame{impl_->context, impl_->theme, info};
}

void UIRenderSystem::drawPanes(Frame &frame)
{
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    if (!impl_->context.frameOpen() || !frame.uses(impl_->context))
    {
        ::lux::ui::detail::failUiContract();
    }
    ScopedImGuiContext context{::lux::ui::detail::ContextAccess::native(impl_->context)};
    struct Placement
    {
        std::string_view id;
        ImVec2 position;
        ImVec2 size;
    };
    std::array<Placement, 5> placements{};
    if (impl_->docking)
    {
        const auto *viewport = ImGui::GetMainViewport();
        const ImGuiID root = ImGui::GetID("lux.ui.dockspace");
        if (impl_->split_layout && !impl_->dock_layout_initialized && viewport->WorkSize.x > 0 &&
            viewport->WorkSize.y > 0)
        {
            impl_->dock_layout_initialized = true;
            const auto &layout = *impl_->split_layout;
            ImGui::DockBuilderRemoveNode(root);
            ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(root, viewport->WorkSize);
            ImGuiID center = root;
            auto &regions = impl_->dock_regions;
            if (!layout.toolbar.empty())
            {
                ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.12F, &regions[4], &center);
            }
            if (!layout.bottom.empty())
            {
                ImGui::DockBuilderSplitNode(center, ImGuiDir_Down,
                                            std::clamp(layout.bottom_height / viewport->WorkSize.y, 0.1F, 0.4F),
                                            &regions[3], &center);
            }
            if (!layout.left.empty())
            {
                ImGui::DockBuilderSplitNode(center, ImGuiDir_Left,
                                            std::clamp(layout.left_width / viewport->WorkSize.x, 0.1F, 0.3F),
                                            &regions[0], &center);
            }
            if (!layout.right.empty())
            {
                ImGui::DockBuilderSplitNode(
                    center, ImGuiDir_Right,
                    std::clamp(layout.right_width / std::max(1.0F, viewport->WorkSize.x - layout.left_width), 0.1F,
                               0.4F),
                    &regions[2], &center);
            }
            regions[1] = center;
            ImGui::DockBuilderFinish(root);
        }
        // Submit even when panes are hidden: docking owns their persistent placement.
        ImGui::DockSpaceOverViewport(root, viewport);
    }
    if (impl_->split_layout && !impl_->docking)
    {
        auto &layout = *impl_->split_layout;
        const auto size = ImGui::GetIO().DisplaySize;
        const float top = layout.toolbar.empty() ? 0 : 38.0F;
        const auto visible = [&](const std::string &id) {
            for (auto &record : impl_->panes)
            {
                const auto *pane = Impl::resolve(record);
                if (pane && pane->id().name() == id)
                {
                    return pane->visible();
                }
            }
            return false;
        };
        const float left =
            size.x >= 700 && visible(layout.left) ? std::clamp(layout.left_width, 160.0F, size.x * 0.3F) : 0;
        const float right =
            size.x >= 1000 && visible(layout.right) ? std::clamp(layout.right_width, 220.0F, size.x * 0.35F) : 0;
        const float bottom =
            size.y >= 450 && visible(layout.bottom) ? std::clamp(layout.bottom_height, 100.0F, size.y * 0.4F) : 0;
        const auto splitter = [&](const char *id, ImVec2 pos, ImVec2 extent, bool vertical, float &value) {
            ImGui::SetNextWindowPos(pos);
            ImGui::SetNextWindowSize(extent);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 0});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2{1, 1});
            ImGui::Begin(id, nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoNav);
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
        placements = {
            {{layout.left, {0, top}, {left, std::max(0.0F, upper_height - top)}},
             {layout.center,
              {center_x, top},
              {std::max(0.0F, size.x - center_x - right - (right > 0 ? 5 : 0)), std::max(0.0F, upper_height - top)}},
             {layout.right, {size.x - right, top}, {right, std::max(0.0F, upper_height - top)}},
             {layout.bottom, {0, size.y - bottom}, {size.x, bottom}},
             {layout.toolbar, {0, 0}, {size.x, top}}}};
    }
    PaneHandle focused_candidate;
    PaneHandle hovered_candidate;
    impl_->frame_focused_contexts.clear();
    for (auto &record : impl_->panes)
    {
        auto *pane = Impl::resolve(record);
        if (!pane)
        {
            continue;
        }
        const auto current = Impl::handle(record);
        if (!pane->visible())
        {
            if (impl_->pending_focus.token == record.token)
            {
                impl_->pending_focus.reset();
            }
            continue;
        }
        bool visible = true;
        ImGuiWindowFlags flags = 0;
        if (impl_->docking && impl_->split_layout)
        {
            const auto &layout = *impl_->split_layout;
            const std::array<std::string_view, 5> ids{layout.left, layout.center, layout.right, layout.bottom,
                                                      layout.toolbar};
            for (std::size_t index{}; index < ids.size(); ++index)
            {
                if (ids[index] == pane->id().name())
                {
                    auto *node = ImGui::DockBuilderGetNode(impl_->dock_regions[index]);
                    if (!node || !node->IsLeafNode())
                    {
                        // User docking may have merged away a default region.
                        node = ImGui::DockBuilderGetCentralNode(ImGui::GetID("lux.ui.dockspace"));
                    }
                    if (node)
                    {
                        ImGui::SetNextWindowDockID(node->ID, ImGuiCond_FirstUseEver);
                    }
                    break;
                }
            }
        }
        bool collapsed_by_layout = false;
        for (const auto &placement : placements)
        {
            if (placement.id.empty() || pane->id().name() != placement.id)
            {
                continue;
            }
            collapsed_by_layout = placement.size.x <= 0 || placement.size.y <= 0;
            if (!collapsed_by_layout)
            {
                ImGui::SetNextWindowPos(placement.position);
                ImGui::SetNextWindowSize(placement.size);
            }
            flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
            break;
        }
        if (collapsed_by_layout)
        {
            continue;
        }
        if (impl_->pending_focus.token == record.token)
        {
            ImGui::SetNextWindowFocus();
            impl_->pending_focus.reset();
        }
        const bool toolbar = impl_->split_layout && pane->id().name() == impl_->split_layout->toolbar;
        if (toolbar && !impl_->docking)
        {
            flags |= ImGuiWindowFlags_NoDecoration;
        }
        impl_->frame_context_scratch.clear();
        auto draw_context = ::lux::ui::detail::PaneStateAccess::context(impl_->frame_context_scratch);
        const bool shown =
            ImGui::Begin(::lux::ui::detail::PaneStateAccess::windowLabel(*pane), toolbar ? nullptr : &visible, flags);
        // A foreign Pane may fail during preparation. Its own scopes unwind first; this scope
        // always balances the outer window before the owning Frame can be discarded.
        struct PaneWindowScope final
        {
            bool open{true};
            void finish() noexcept
            {
                if (std::exchange(open, false))
                {
                    ImGui::End();
                }
            }
            ~PaneWindowScope() noexcept
            {
                finish();
            }
        } window_scope;
        if (shown)
        {
            ::lux::ui::detail::PaneStateAccess::draw(*pane, frame, draw_context);
            pane = Impl::resolve(record);
            if (pane && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            {
                focused_candidate = current;
                impl_->frame_focused_contexts = impl_->frame_context_scratch;
            }
            if (pane && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
            {
                hovered_candidate = current;
            }
        }
        window_scope.finish();
        pane = Impl::resolve(record);
        if (pane)
        {
            pane->setVisible(visible);
        }
    }
    impl_->commitFocus(focused_candidate, impl_->frame_focused_contexts);
    impl_->commitHover(hovered_candidate);
}

LayoutSnapshot UIRenderSystem::captureLayout() const
{
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    ScopedImGuiContext context{::lux::ui::detail::ContextAccess::native(impl_->context)};
    std::size_t size = 0;
    const char *data = ImGui::SaveIniSettingsToMemory(&size);
    std::vector<std::byte> bytes(size);
    if (size != 0)
    {
        std::memcpy(bytes.data(), data, size);
    }
    return LayoutSnapshot{std::move(bytes)};
}

UiInputSnapshot UIRenderSystem::inputSnapshot() const noexcept
{
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    ScopedImGuiContext context{::lux::ui::detail::ContextAccess::native(impl_->context)};
    UiInputSnapshot result;
    const auto &io = ImGui::GetIO();
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
    result.modal_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
    result.keyboard_blocked = io.WantTextInput || ImGui::IsAnyItemActive() || result.modal_open;
    return result;
}

void UIRenderSystem::setSplitLayout(SplitLayout layout)
{
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    if (!std::isfinite(layout.left_width) || !std::isfinite(layout.right_width) || !std::isfinite(layout.bottom_height))
    {
        return;
    }
    impl_->split_layout = std::move(layout);
}

lux::cxx::expected<void, ELayoutError> UIRenderSystem::validateSplitLayout(const SplitLayout &layout) const noexcept
{
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    const std::array<std::string_view, 5> ids{layout.left, layout.center, layout.right, layout.bottom, layout.toolbar};
    const bool valid_dimensions = std::isfinite(layout.left_width) && layout.left_width >= 0 &&
                                  std::isfinite(layout.right_width) && layout.right_width >= 0 &&
                                  std::isfinite(layout.bottom_height) && layout.bottom_height >= 0;
    if (!valid_dimensions || layout.center.empty())
    {
        return lux::cxx::unexpected(ELayoutError::INVALID_DATA);
    }
    for (std::size_t index = 0; index < ids.size(); ++index)
    {
        if (ids[index].empty())
        {
            continue;
        }
        if (std::find(ids.begin(), ids.begin() + index, ids[index]) != ids.begin() + index)
        {
            return lux::cxx::unexpected(ELayoutError::INVALID_DATA);
        }
        const auto matches = [&](const auto &record) {
            return !record.tombstone && record.id.name() == ids[index] && record.lifetime.alive();
        };
        if (!std::any_of(impl_->panes.begin(), impl_->panes.end(), matches) &&
            !std::any_of(impl_->pending_panes.begin(), impl_->pending_panes.end(), matches))
        {
            return lux::cxx::unexpected(ELayoutError::INVALID_DATA);
        }
    }
    return {};
}

void UIRenderSystem::clearSplitLayout()
{
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    impl_->split_layout.reset();
}

lux::cxx::expected<void, ELayoutError> UIRenderSystem::restoreLayout(const LayoutSnapshot &snapshot)
{
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    const auto bytes = snapshot.bytes();
    if (bytes.empty())
    {
        return lux::cxx::unexpected<ELayoutError>{ELayoutError::INVALID_DATA};
    }
    ScopedImGuiContext context{::lux::ui::detail::ContextAccess::native(impl_->context)};
    ImGui::LoadIniSettingsFromMemory(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (impl_->docking &&
        std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()).find("[Docking][Data]") !=
            std::string_view::npos)
    {
        impl_->dock_layout_initialized = true;
    }
    return {};
}

void UIRenderSystem::unregisterPane(std::uint64_t token) noexcept
{
    LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
    auto *found = impl_->findPane(token);
    if (!found || found->tombstone)
    {
        return;
    }
    found->tombstone = true;
    if (impl_->pending_focus.token == token)
    {
        impl_->pending_focus.reset();
    }
    if (impl_->focused_pane.token == token)
    {
        impl_->clearFocus();
    }
    if (impl_->hovered_pane.token == token)
    {
        impl_->commitHover({});
    }
    if (!impl_->context.frameOpen())
    {
        impl_->compactPaneRecords();
    }
}

scene::SceneStageResult UIRenderSystem::maintainUi() noexcept
{
    if (impl_->capture_state == Impl::CaptureState::BUILDING && !impl_->context.frameOpen())
    {
        // An unfinished Frame ended its CPU work without producing output.
        impl_->capture_state = Impl::CaptureState::EMPTY;
        impl_->current_frame = impl_->frames.size();
    }
    for (std::size_t index = 0; index < impl_->frames.size(); ++index)
    {
        auto &slot = impl_->frames[index];
        if (index != impl_->current_frame && slot.use_count() == 1)
        {
            // Retain storage capacity, not sampled image leases. Otherwise
            // an idle returned slot would keep a closed document alive.
            slot->images.clear();
        }
    }
    return maintain();
}

bool UIRenderSystem::canBuildFrame() const noexcept
{
    return !impl_->frames_stopped && impl_->publication && impl_->capture_state == Impl::CaptureState::EMPTY &&
           !impl_->context.frameOpen() &&
           std::ranges::any_of(impl_->frames, [](const auto &slot) { return slot.use_count() == 1; });
}

void UIRenderSystem::stopFrames() noexcept
{
    // Called only after the current Frame has ended at the owner boundary.
    // Cancel unaccepted output; accepted Programs retain their own snapshot.
    assert(!impl_->context.frameOpen());
    if (impl_->frames_stopped)
    {
        return;
    }
    impl_->frames_stopped = true;
    impl_->clear_pending = true;
    impl_->packet.clear_keep_capacity();
    impl_->capture_state = Impl::CaptureState::EMPTY;
    impl_->current_frame = impl_->frames.size();
    for (auto &slot : impl_->frames)
    {
        if (slot.use_count() == 1)
        {
            slot->images.clear();
        }
    }
}

lux::cxx::expected<void, EUiCaptureError> UIRenderSystem::finishFrame(Frame &frame,
                                                                      std::span<const render::ViewImage> images)
{
    if (impl_->capture_state != Impl::CaptureState::BUILDING || !frame.uses(impl_->context))
    {
        return lux::cxx::unexpected(EUiCaptureError::FRAME_OPEN);
    }
    frame.finish();
    auto &slot = *impl_->frames[impl_->current_frame];
    auto captured = impl_->context.capture(slot.snapshot);
    if (!captured)
    {
        impl_->capture_state = Impl::CaptureState::EMPTY;
        return lux::cxx::unexpected(captured.error());
    }
    slot.images.assign(images.begin(), images.end());
    slot.sequence = ++impl_->captures;
    impl_->capture_state = Impl::CaptureState::CAPTURED;
    if (impl_->window_focused)
    {
        impl_->captured_anchor = impl_->backend_anchor;
        impl_->captured_anchor.frame = impl_->frame_sequence;
    }
    return {};
}

scene::SceneStageResult UIRenderSystem::publishFrame(scene::SceneStageContext &context) noexcept
{
    if (!impl_->publication)
    {
        return impl_->publication;
    }
    auto ready = maintain();
    if (!ready || *ready == scene::ESceneProgress::PENDING)
    {
        return ready;
    }
    if (impl_->clear_pending)
    {
        if (impl_->packet.payload.empty())
        {
            render::RenderProgramSession::Builder builder(impl_->packet);
            builder.begin({});
            impl_->packet.kind = render::ERenderProgramKind::StateUpdate;
            const auto appended =
                appendUiClear(builder, impl_->operations, sceneLease(), feature(render::kUiRenderDescriptor.type));
            if (!appended)
            {
                impl_->publication = lux::cxx::unexpected(scene::SceneExecutionFailure{
                    scene::ESceneExecutionError::SYSTEM_FAILURE, instanceId(), appended.error()});
                return impl_->publication;
            }
        }
        const auto submitted = submitProgram(impl_->packet, context);
        if (submitted && *submitted == scene::ESceneProgress::COMPLETE)
        {
            impl_->clear_pending = false;
        }
        return submitted;
    }
    if (impl_->capture_state == Impl::CaptureState::EMPTY)
    {
        return scene::ESceneProgress::COMPLETE;
    }
    if (impl_->capture_state == Impl::CaptureState::BUILDING)
    {
        return scene::ESceneProgress::PENDING;
    }
    if (impl_->capture_state == Impl::CaptureState::CAPTURED)
    {
        render::RenderProgramSession::Builder builder(impl_->packet);
        builder.begin({});
        impl_->packet.kind = render::ERenderProgramKind::Frame;
        auto appended = appendUiFrame(builder, impl_->operations, sceneLease(),
                                      feature(render::kUiRenderDescriptor.type), impl_->frames[impl_->current_frame]);
        if (!appended)
        {
            impl_->publication = lux::cxx::unexpected(scene::SceneExecutionFailure{
                scene::ESceneExecutionError::SYSTEM_FAILURE, instanceId(), appended.error()});
            return impl_->publication;
        }
        impl_->capture_state = Impl::CaptureState::PACKED;
    }
    auto submitted = submitProgram(impl_->packet, context);
    if (submitted && *submitted == scene::ESceneProgress::COMPLETE)
    {
        impl_->capture_state = Impl::CaptureState::EMPTY;
        impl_->current_frame = impl_->frames.size();
    }
    return submitted;
}

std::uint64_t UIRenderSystem::capturedFrames() const noexcept
{
    return impl_->captures;
}

UiTextInputAnchor UIRenderSystem::textInputAnchor() const noexcept
{
    if (std::this_thread::get_id() != control_->owner)
    {
        return {};
    }
    return impl_->context.frameOpen() ? UiTextInputAnchor{} : impl_->captured_anchor;
}

lux::cxx::expected<void, scene::SceneSystemBuildFailure> installUIRenderSystem(
    scene::SceneBuilder &builder, scene::SceneSystemDescription description) noexcept
{
    auto *runtime = builder.require<render::RenderRuntime>(description.instanceId(), "render_runtime");
    auto *dispatcher = builder.require<object::ObjectDispatcherRef>(description.instanceId(), "dispatcher");
    auto *config = builder.require<UIRenderSystemConfig>(description.instanceId(), "ui_config");
    if (!runtime || !dispatcher || !*dispatcher || !dispatcher->isCurrent() || !config)
    {
        return lux::cxx::unexpected(scene::SceneSystemBuildFailure{scene::ESceneSystemBuildError::MISSING_REQUIREMENT,
                                                                   description.instanceId()});
    }
    auto cpu = Context::create({config->docking}, config->font);
    if (!cpu)
    {
        return lux::cxx::unexpected(scene::SceneSystemBuildFailure{
            scene::ESceneSystemBuildError::CONSTRUCTION_FAILURE, description.instanceId(), {}, 0, {}, cpu.error()});
    }
    auto wire = makeUiRenderConfiguration(*cpu);
    if (!wire)
    {
        return lux::cxx::unexpected(scene::SceneSystemBuildFailure{
            scene::ESceneSystemBuildError::CONSTRUCTION_FAILURE, description.instanceId(), {}, 0, {}, wire.error()});
    }
    const auto type = runtime->features().typeId("UiRender");
    if (!type)
    {
        return lux::cxx::unexpected(scene::SceneSystemBuildFailure{scene::ESceneSystemBuildError::MISSING_REQUIREMENT,
                                                                   description.instanceId()});
    }
    auto lease =
        runtime->createScene({.name = "Editor UI"}, {{render::kUiRenderDescriptor.type, type, std::move(*wire), {}}});
    if (!lease)
    {
        return lux::cxx::unexpected(
            scene::SceneSystemBuildFailure{scene::ESceneSystemBuildError::EXTERNAL_OPERATION_FAILURE,
                                           description.instanceId(),
                                           {},
                                           0,
                                           {},
                                           lease.error()});
    }
    auto value = builder.emplaceSystem<UIRenderSystem>(description.instanceId(), description.instanceId(),
                                                       builder.sceneInstanceId(), *runtime, builder.registry(),
                                                       std::move(*lease), std::move(*cpu), *config, *dispatcher);
    if (!value)
    {
        return lux::cxx::unexpected(value.error());
    }
    auto maintained = builder.addMaintenanceTask<UIRenderSystem>(
        description.instanceId(), [](UIRenderSystem &self) noexcept { return self.maintainUi(); });
    if (!maintained)
    {
        return maintained;
    }
    return builder.addPublicationTask<UIRenderSystem>(
        description.instanceId(),
        [](UIRenderSystem &self, scene::SceneStageContext &turn) noexcept { return self.publishFrame(turn); });
}

scene::SceneSystemRegistration uiRenderSystemRegistration() noexcept
{
    static constexpr std::array requirements{
        scene::SceneSystemRequirementSpec{"render_runtime", "lux.render.runtime",
                                          cxx::typeToken<render::RenderRuntime>(), false},
        scene::SceneSystemRequirementSpec{"dispatcher", "lux.object.dispatcher",
                                          cxx::typeToken<object::ObjectDispatcherRef>(), false},
        scene::SceneSystemRequirementSpec{"ui_config", "lux.editor.ui.config", cxx::typeToken<UIRenderSystemConfig>(),
                                          false}};
    static constexpr std::array projections{
        scene::sceneSystemCapabilityProjection<UIRenderSystem, scene::RenderSystem>()};
    return {.type = system::systemTypeId(UIRenderSystem::Description.canonical_name),
            .cpp_type = cxx::typeToken<UIRenderSystem>(),
            .description = &UIRenderSystem::Description,
            .requirements = requirements,
            .project_object = scene::sceneSystemObjectProjection<UIRenderSystem>(),
            .install = &installUIRenderSystem,
            .projections = projections};
}
} // namespace lux::editor::ui
