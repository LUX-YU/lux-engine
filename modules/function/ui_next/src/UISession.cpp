#include <lux/engine/ui_next/UISession.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::ui
{
    namespace detail
    {
        struct SessionControl final
        {
            UISession* session{nullptr};
        };

        struct PaneStateAccess final
        {
            static void setFocused(Pane& pane, bool focused)
            {
                pane.setFocused(focused);
            }

            static void setHovered(Pane& pane, bool hovered) noexcept
            {
                pane.setHovered(hovered);
            }
        };
    } // namespace detail

    namespace
    {
        class ScopedImGuiContext final
        {
        public:
            explicit ScopedImGuiContext(ImGuiContext* target) noexcept
                : previous_(ImGui::GetCurrentContext())
            {
                ImGui::SetCurrentContext(target);
            }

            ~ScopedImGuiContext() { ImGui::SetCurrentContext(previous_); }

        private:
            ImGuiContext* previous_{nullptr};
        };

        struct PaneRecord final
        {
            std::uint64_t token{0};
            Pane* pane{nullptr};
        };

        struct FactoryRecord final
        {
            std::uint64_t token{0};
            PaneFactory factory;
        };

        [[nodiscard]] ImGuiKey toImGuiKey(EUiKey key) noexcept
        {
            switch (key)
            {
            case EUiKey::TAB:
                return ImGuiKey_Tab;
            case EUiKey::LEFT_ARROW:
                return ImGuiKey_LeftArrow;
            case EUiKey::RIGHT_ARROW:
                return ImGuiKey_RightArrow;
            case EUiKey::UP_ARROW:
                return ImGuiKey_UpArrow;
            case EUiKey::DOWN_ARROW:
                return ImGuiKey_DownArrow;
            case EUiKey::PAGE_UP:
                return ImGuiKey_PageUp;
            case EUiKey::PAGE_DOWN:
                return ImGuiKey_PageDown;
            case EUiKey::HOME:
                return ImGuiKey_Home;
            case EUiKey::END:
                return ImGuiKey_End;
            case EUiKey::INSERT:
                return ImGuiKey_Insert;
            case EUiKey::DELETE_KEY:
                return ImGuiKey_Delete;
            case EUiKey::BACKSPACE:
                return ImGuiKey_Backspace;
            case EUiKey::SPACE:
                return ImGuiKey_Space;
            case EUiKey::ENTER:
                return ImGuiKey_Enter;
            case EUiKey::ESCAPE:
                return ImGuiKey_Escape;
            case EUiKey::A:
                return ImGuiKey_A;
            case EUiKey::C:
                return ImGuiKey_C;
            case EUiKey::V:
                return ImGuiKey_V;
            case EUiKey::X:
                return ImGuiKey_X;
            case EUiKey::Y:
                return ImGuiKey_Y;
            case EUiKey::Z:
                return ImGuiKey_Z;
            case EUiKey::LEFT_CTRL:
                return ImGuiKey_LeftCtrl;
            case EUiKey::LEFT_SHIFT:
                return ImGuiKey_LeftShift;
            case EUiKey::LEFT_ALT:
                return ImGuiKey_LeftAlt;
            case EUiKey::LEFT_SUPER:
                return ImGuiKey_LeftSuper;
            case EUiKey::RIGHT_CTRL:
                return ImGuiKey_RightCtrl;
            case EUiKey::RIGHT_SHIFT:
                return ImGuiKey_RightShift;
            case EUiKey::RIGHT_ALT:
                return ImGuiKey_RightAlt;
            case EUiKey::RIGHT_SUPER:
                return ImGuiKey_RightSuper;
            case EUiKey::NONE:
                break;
            }
            return ImGuiKey_None;
        }
    } // namespace

    struct UISession::Impl final
    {
        ImGuiContext* context{nullptr};
        lux::object::ObjectMessageQueue messages;
        CommandRouter command_router;
        std::vector<PaneRecord> panes;
        std::vector<FactoryRecord> factories;
        std::vector<UiContextIdView> focused_local_contexts;
        std::vector<UiContextIdView> focused_contexts;
        std::vector<UiContextIdView> frame_context_scratch;
        std::vector<UiContextIdView> frame_focused_contexts;
        Pane* focused_pane{nullptr};
        Pane* hovered_pane{nullptr};
        std::uint64_t next_token{1};
        bool frame_open{false};

        void rebuildFocusedContexts()
        {
            focused_contexts.clear();
            const auto append_unique = [&](UiContextIdView context)
            {
                if (!context.isValid())
                    return;
                if (std::ranges::find(focused_contexts, context) ==
                    focused_contexts.end())
                {
                    focused_contexts.push_back(context);
                }
            };

            for (auto iterator = focused_local_contexts.rbegin();
                 iterator != focused_local_contexts.rend();
                 ++iterator)
            {
                append_unique(*iterator);
            }
            if (focused_pane)
            {
                for (const auto context : focused_pane->contexts())
                    append_unique(context);
            }
            append_unique(kGlobalContext);
            command_router.setActiveContexts(focused_contexts);
        }

        void commitFocus(Pane* pane, std::span<const UiContextIdView> local_contexts)
        {
            if (focused_pane != pane)
            {
                if (focused_pane)
                    detail::PaneStateAccess::setFocused(*focused_pane, false);
                focused_pane = pane;
                if (focused_pane)
                    detail::PaneStateAccess::setFocused(*focused_pane, true);
                command_router.setActivationScope(focused_pane);
            }
            focused_local_contexts.assign(local_contexts.begin(), local_contexts.end());
            rebuildFocusedContexts();
        }

        void commitHover(Pane* pane)
        {
            if (hovered_pane == pane)
                return;
            if (hovered_pane)
                detail::PaneStateAccess::setHovered(*hovered_pane, false);
            hovered_pane = pane;
            if (hovered_pane)
                detail::PaneStateAccess::setHovered(*hovered_pane, true);
        }
    };

    UISession::Registration::Registration(
        std::weak_ptr<detail::SessionControl> control,
        std::uint64_t token,
        EKind kind
    ) noexcept
        : control_(std::move(control)), token_(token), kind_(kind)
    {
    }

    UISession::Registration::Registration(Registration&& other) noexcept
        : control_(std::move(other.control_)), token_(std::exchange(other.token_, 0)),
          kind_(other.kind_)
    {
    }

    UISession::Registration& UISession::Registration::operator=(Registration&& other
    ) noexcept
    {
        if (this != std::addressof(other))
        {
            reset();
            control_ = std::move(other.control_);
            token_ = std::exchange(other.token_, 0);
            kind_ = other.kind_;
        }
        return *this;
    }

    UISession::Registration::~Registration() { reset(); }

    void UISession::Registration::reset() noexcept
    {
        if (token_ == 0)
            return;
        if (const auto control = control_.lock(); control && control->session)
            control->session->unregister(token_, kind_);
        token_ = 0;
        control_.reset();
    }

    UISession::UISession()
        : impl_(std::make_unique<Impl>()),
          control_(std::make_shared<detail::SessionControl>())
    {
        auto* previous = ImGui::GetCurrentContext();
        impl_->context = ImGui::CreateContext();
        {
            ScopedImGuiContext context{impl_->context};
            unsigned char* pixels = nullptr;
            int width = 0;
            int height = 0;
            ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        }
        ImGui::SetCurrentContext(previous);
        control_->session = this;
        impl_->focused_contexts.reserve(8);
        impl_->focused_local_contexts.reserve(8);
        impl_->frame_context_scratch.reserve(8);
        impl_->frame_focused_contexts.reserve(8);
        impl_->rebuildFocusedContexts();
    }

    UISession::~UISession()
    {
        control_->session = nullptr;
        impl_->commitFocus(nullptr, {});
        impl_->commitHover(nullptr);
        impl_->messages.close();
        auto* previous = ImGui::GetCurrentContext();
        if (previous == impl_->context)
            previous = nullptr;
        ImGui::SetCurrentContext(impl_->context);
        ImGui::DestroyContext(impl_->context);
        ImGui::SetCurrentContext(previous);
    }

    lux::cxx::expected<UISession::Registration, EUiRegistrationError>
    UISession::registerPane(Pane& pane)
    {
        if (!pane.id().isValid())
        {
            return lux::cxx::unexpected<EUiRegistrationError>{
                EUiRegistrationError::INVALID_ID
            };
        }
        if (std::ranges::any_of(
                impl_->panes,
                [&](const auto& record) { return record.pane->id() == pane.id(); }
            ))
        {
            return lux::cxx::unexpected<EUiRegistrationError>{
                EUiRegistrationError::DUPLICATE_PANE_ID
            };
        }
        if (pane.dispatcherRef() != impl_->messages.dispatcherRef())
        {
            return lux::cxx::unexpected<EUiRegistrationError>{
                EUiRegistrationError::INVALID_ID
            };
        }
        const auto token = impl_->next_token++;
        impl_->panes.push_back({token, std::addressof(pane)});
        return Registration{control_, token, Registration::EKind::PANE};
    }

    lux::cxx::expected<UISession::Registration, EUiRegistrationError>
    UISession::registerFactory(PaneFactory factory)
    {
        if (!factory.type.isValid() || !factory.create)
        {
            return lux::cxx::unexpected<EUiRegistrationError>{
                EUiRegistrationError::INVALID_ID
            };
        }
        if (std::ranges::any_of(
                impl_->factories,
                [&](const auto& record) { return record.factory.type == factory.type; }
            ))
        {
            return lux::cxx::unexpected<EUiRegistrationError>{
                EUiRegistrationError::DUPLICATE_FACTORY
            };
        }
        const auto token = impl_->next_token++;
        impl_->factories.push_back({token, std::move(factory)});
        return Registration{control_, token, Registration::EKind::FACTORY};
    }

    std::unique_ptr<Pane> UISession::createPane(PaneTypeIdView type, PaneId id)
    {
        const auto found = std::ranges::find_if(
            impl_->factories,
            [&](const FactoryRecord& record)
            { return record.factory.type.view() == type; }
        );
        if (found == impl_->factories.end())
            return {};
        return found->factory.create(
            PaneCreateContext{impl_->messages.dispatcherRef()},
            std::move(id)
        );
    }

    CommandRouter& UISession::commandRouter() noexcept { return impl_->command_router; }

    const CommandRouter& UISession::commandRouter() const noexcept
    {
        return impl_->command_router;
    }

    lux::object::ObjectDispatcherRef UISession::dispatcherRef() const noexcept
    {
        return impl_->messages.dispatcherRef();
    }

    bool UISession::focusPane(PaneIdView pane)
    {
        const auto found = std::ranges::find_if(
            impl_->panes,
            [&](const PaneRecord& record) { return record.pane->id().view() == pane; }
        );
        if (found == impl_->panes.end())
            return false;
        impl_->commitFocus(found->pane, {});
        ScopedImGuiContext context{impl_->context};
        ImGui::SetWindowFocus(found->pane->imguiLabel().data());
        return true;
    }

    Pane* UISession::focusedPane() const noexcept { return impl_->focused_pane; }

    std::span<const UiContextIdView> UISession::focusedContexts() const noexcept
    {
        return impl_->focused_contexts;
    }

    void UISession::feedInput(const UiInputEvent& event)
    {
        ScopedImGuiContext context{impl_->context};
        auto& io = ImGui::GetIO();
        std::visit(
            [&](const auto& value)
            {
                using Value = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<Value, UiPointerMove>)
                {
                    io.AddMousePosEvent(value.x, value.y);
                }
                else if constexpr (std::same_as<Value, UiPointerButton>)
                {
                    int button = 0;
                    switch (value.button)
                    {
                    case EUiPointerButton::LEFT:
                        button = 0;
                        break;
                    case EUiPointerButton::RIGHT:
                        button = 1;
                        break;
                    case EUiPointerButton::MIDDLE:
                        button = 2;
                        break;
                    case EUiPointerButton::EXTRA_1:
                        button = 3;
                        break;
                    case EUiPointerButton::EXTRA_2:
                        button = 4;
                        break;
                    }
                    io.AddMouseButtonEvent(button, value.down);
                }
                else if constexpr (std::same_as<Value, UiPointerWheel>)
                {
                    io.AddMouseWheelEvent(value.horizontal, value.vertical);
                }
                else if constexpr (std::same_as<Value, UiKey>)
                {
                    const auto key = toImGuiKey(value.key);
                    if (key != ImGuiKey_None)
                        io.AddKeyEvent(key, value.down);
                }
                else if constexpr (std::same_as<Value, UiText>)
                {
                    io.AddInputCharacter(static_cast<unsigned int>(value.codepoint));
                }
                else if constexpr (std::same_as<Value, UiWindowFocus>)
                {
                    io.AddFocusEvent(value.focused);
                }
            },
            event
        );
    }

    void UISession::beginFrame(ImVec2 display_size, float delta_seconds)
    {
        assert(!impl_->frame_open);
        ScopedImGuiContext context{impl_->context};
        static_cast<void>(impl_->messages.dispatchPending());
        auto& io = ImGui::GetIO();
        io.DisplaySize = display_size;
        io.DeltaTime = delta_seconds;
        ImGui::NewFrame();
        impl_->frame_open = true;
    }

    void UISession::drawPanes()
    {
        assert(impl_->frame_open);
        ScopedImGuiContext context{impl_->context};
        Pane* focused_candidate = nullptr;
        Pane* hovered_candidate = nullptr;
        impl_->frame_focused_contexts.clear();
        for (auto& record : impl_->panes)
        {
            auto& pane = *record.pane;
            if (!pane.visible())
                continue;
            bool visible = true;
            impl_->frame_context_scratch.clear();
            PaneDrawContext draw_context{impl_->frame_context_scratch};
            if (ImGui::Begin(pane.imguiLabel().data(), &visible))
            {
                pane.draw(draw_context);
                if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
                {
                    focused_candidate = std::addressof(pane);
                    impl_->frame_focused_contexts = impl_->frame_context_scratch;
                }
                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
                {
                    hovered_candidate = std::addressof(pane);
                }
            }
            ImGui::End();
            pane.setVisible(visible);
        }
        impl_->commitFocus(focused_candidate, impl_->frame_focused_contexts);
        impl_->commitHover(hovered_candidate);
    }

    ImDrawData* UISession::endFrame()
    {
        assert(impl_->frame_open);
        ScopedImGuiContext context{impl_->context};
        ImGui::Render();
        impl_->frame_open = false;
        return ImGui::GetDrawData();
    }

    LayoutSnapshot UISession::captureLayout() const
    {
        ScopedImGuiContext context{impl_->context};
        std::size_t size = 0;
        const char* data = ImGui::SaveIniSettingsToMemory(&size);
        LayoutSnapshot layout;
        layout.bytes.resize(size);
        if (size != 0)
            std::memcpy(layout.bytes.data(), data, size);
        return layout;
    }

    lux::cxx::expected<void, ELayoutError>
    UISession::restoreLayout(std::span<const std::byte> bytes)
    {
        if (bytes.empty())
        {
            return lux::cxx::unexpected<ELayoutError>{ELayoutError::INVALID_DATA};
        }
        ScopedImGuiContext context{impl_->context};
        ImGui::LoadIniSettingsFromMemory(
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size()
        );
        return {};
    }

    ImGuiContext* UISession::imguiContext() const noexcept { return impl_->context; }

    void UISession::unregister(std::uint64_t token, Registration::EKind kind) noexcept
    {
        if (kind == Registration::EKind::PANE)
        {
            const auto found =
                std::ranges::find(impl_->panes, token, &PaneRecord::token);
            if (found == impl_->panes.end())
                return;
            if (impl_->focused_pane == found->pane)
                impl_->commitFocus(nullptr, {});
            if (impl_->hovered_pane == found->pane)
                impl_->commitHover(nullptr);
            impl_->panes.erase(found);
        }
        else
        {
            std::erase_if(
                impl_->factories,
                [token](const FactoryRecord& record) { return record.token == token; }
            );
        }
    }
} // namespace lux::ui
