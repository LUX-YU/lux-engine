#include <lux/engine/ui_next/UISession.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
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
    }

    namespace
    {
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

        [[nodiscard]] std::string paneLabel(const Pane& pane)
        {
            std::string label{pane.title()};
            label += "###";
            label += pane.id().name();
            return label;
        }
    }

    struct UISession::Impl final
    {
        ImGuiContext* context{nullptr};
        lux::object::ObjectDispatcher dispatcher;
        CommandRouter command_router;
        std::vector<PaneRecord> panes;
        std::vector<FactoryRecord> factories;
        std::vector<UiContextId> focused_local_contexts;
        std::vector<UiContextIdView> focused_contexts;
        Pane* focused_pane{nullptr};
        Pane* hovered_pane{nullptr};
        std::uint64_t next_token{1};
        bool frame_open{false};

        void rebuildFocusedContexts()
        {
            focused_contexts.clear();
            const auto append_unique = [&](UiContextIdView context)
            {
                if (!context.isValid()) return;
                if (std::ranges::find(focused_contexts, context)
                    == focused_contexts.end())
                {
                    focused_contexts.push_back(context);
                }
            };

            for (auto iterator = focused_local_contexts.rbegin();
                iterator != focused_local_contexts.rend();
                ++iterator)
            {
                append_unique(iterator->view());
            }
            if (focused_pane)
            {
                for (const auto& context_id : focused_pane->contexts())
                    append_unique(context_id.view());
            }
            append_unique(kGlobalContext);
            command_router.setActiveContexts(focused_contexts);
        }

        void commitFocus(Pane* pane, std::vector<UiContextId> local_contexts)
        {
            if (focused_pane != pane)
            {
                if (focused_pane)
                    detail::PaneStateAccess::setFocused(*focused_pane, false);
                focused_pane = pane;
                if (focused_pane)
                    detail::PaneStateAccess::setFocused(*focused_pane, true);
            }
            focused_local_contexts = std::move(local_contexts);
            command_router.setFocusedReceiver(focused_pane);
            rebuildFocusedContexts();
        }

        void commitHover(Pane* pane)
        {
            if (hovered_pane == pane) return;
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
        : control_(std::move(other.control_)),
          token_(std::exchange(other.token_, 0)),
          kind_(other.kind_)
    {
    }

    UISession::Registration& UISession::Registration::operator=(
        Registration&& other
    ) noexcept
    {
        if (this != &other)
        {
            reset();
            control_ = std::move(other.control_);
            token_ = std::exchange(other.token_, 0);
            kind_ = other.kind_;
        }
        return *this;
    }

    UISession::Registration::~Registration()
    {
        reset();
    }

    void UISession::Registration::reset() noexcept
    {
        if (token_ == 0) return;
        if (const auto control = control_.lock(); control && control->session)
            control->session->unregister(token_, kind_);
        token_ = 0;
        control_.reset();
    }

    UISession::UISession()
        : impl_(std::make_unique<Impl>()),
          control_(std::make_shared<detail::SessionControl>())
    {
        impl_->context = ImGui::CreateContext();
        ImGui::SetCurrentContext(impl_->context);
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        control_->session = this;
        impl_->rebuildFocusedContexts();
    }

    UISession::~UISession()
    {
        control_->session = nullptr;
        impl_->commitFocus(nullptr, {});
        impl_->commitHover(nullptr);
        impl_->dispatcher.close();
        ImGui::DestroyContext(impl_->context);
    }

    lux::cxx::expected<UISession::Registration, EUiRegistrationError>
    UISession::registerPane(Pane& pane)
    {
        if (!pane.id().isValid())
            return lux::cxx::unexpected(EUiRegistrationError::INVALID_ID);
        if (std::ranges::any_of(impl_->panes, [&](const auto& record)
            { return record.pane->id() == pane.id(); }))
        {
            return lux::cxx::unexpected(
                EUiRegistrationError::DUPLICATE_PANE_ID
            );
        }
        if (pane.dispatcher() != std::addressof(impl_->dispatcher))
            return lux::cxx::unexpected(EUiRegistrationError::INVALID_ID);
        const auto token = impl_->next_token++;
        impl_->panes.push_back({token, &pane});
        return Registration{control_, token, Registration::EKind::PANE};
    }

    lux::cxx::expected<UISession::Registration, EUiRegistrationError>
    UISession::registerFactory(PaneFactory factory)
    {
        if (!factory.type.isValid() || !factory.create)
            return lux::cxx::unexpected(EUiRegistrationError::INVALID_ID);
        if (std::ranges::any_of(impl_->factories, [&](const auto& record)
            { return record.factory.type == factory.type; }))
        {
            return lux::cxx::unexpected(
                EUiRegistrationError::DUPLICATE_FACTORY
            );
        }
        const auto token = impl_->next_token++;
        impl_->factories.push_back({token, std::move(factory)});
        return Registration{control_, token, Registration::EKind::FACTORY};
    }

    std::unique_ptr<Pane> UISession::createPane(
        PaneTypeIdView type,
        PaneId id
    )
    {
        const auto found = std::ranges::find_if(
            impl_->factories,
            [&](const FactoryRecord& record)
            {
                return record.factory.type.view() == type;
            }
        );
        if (found == impl_->factories.end()) return {};
        return found->factory.create(
            PaneCreateContext{impl_->dispatcher},
            std::move(id)
        );
    }

    CommandRouter& UISession::commandRouter() noexcept
    {
        return impl_->command_router;
    }

    const CommandRouter& UISession::commandRouter() const noexcept
    {
        return impl_->command_router;
    }

    lux::object::ObjectDispatcher& UISession::dispatcher() noexcept
    {
        return impl_->dispatcher;
    }

    bool UISession::focusPane(PaneIdView pane)
    {
        const auto found = std::ranges::find_if(
            impl_->panes,
            [&](const PaneRecord& record)
            {
                return record.pane->id().view() == pane;
            }
        );
        if (found == impl_->panes.end()) return false;
        impl_->commitFocus(found->pane, {});
        ImGui::SetCurrentContext(impl_->context);
        ImGui::SetWindowFocus(paneLabel(*found->pane).c_str());
        return true;
    }

    Pane* UISession::focusedPane() const noexcept
    {
        return impl_->focused_pane;
    }

    std::span<const UiContextIdView> UISession::focusedContexts() const noexcept
    {
        return impl_->focused_contexts;
    }

    void UISession::beginFrame(ImVec2 display_size, float delta_seconds)
    {
        assert(!impl_->frame_open);
        static_cast<void>(impl_->dispatcher.dispatchPending());
        ImGui::SetCurrentContext(impl_->context);
        auto& io = ImGui::GetIO();
        io.DisplaySize = display_size;
        io.DeltaTime = delta_seconds;
        ImGui::NewFrame();
        impl_->frame_open = true;
    }

    void UISession::drawPanes()
    {
        assert(impl_->frame_open);
        Pane* focused_candidate = nullptr;
        Pane* hovered_candidate = nullptr;
        std::vector<UiContextId> focused_local_contexts;
        for (auto& record : impl_->panes)
        {
            auto& pane = *record.pane;
            if (!pane.visible()) continue;
            bool visible = true;
            const auto label = paneLabel(pane);
            std::vector<UiContextId> local_contexts;
            PaneDrawContext draw_context{local_contexts};
            if (ImGui::Begin(label.c_str(), &visible))
            {
                pane.draw(draw_context);
                if (ImGui::IsWindowFocused(
                    ImGuiFocusedFlags_RootAndChildWindows
                ))
                {
                    focused_candidate = &pane;
                    focused_local_contexts = std::move(local_contexts);
                }
                if (ImGui::IsWindowHovered(
                    ImGuiHoveredFlags_RootAndChildWindows
                ))
                {
                    hovered_candidate = &pane;
                }
            }
            ImGui::End();
            pane.setVisible(visible);
        }
        impl_->commitFocus(
            focused_candidate,
            std::move(focused_local_contexts)
        );
        impl_->commitHover(hovered_candidate);
    }

    ImDrawData* UISession::endFrame()
    {
        assert(impl_->frame_open);
        ImGui::SetCurrentContext(impl_->context);
        ImGui::Render();
        impl_->frame_open = false;
        return ImGui::GetDrawData();
    }

    LayoutSnapshot UISession::captureLayout() const
    {
        ImGui::SetCurrentContext(impl_->context);
        std::size_t size = 0;
        const char* data = ImGui::SaveIniSettingsToMemory(&size);
        LayoutSnapshot layout;
        layout.bytes.resize(size);
        if (size != 0) std::memcpy(layout.bytes.data(), data, size);
        return layout;
    }

    lux::cxx::expected<void, ELayoutError> UISession::restoreLayout(
        std::span<const std::byte> bytes
    )
    {
        if (bytes.empty())
            return lux::cxx::unexpected(ELayoutError::INVALID_DATA);
        ImGui::SetCurrentContext(impl_->context);
        ImGui::LoadIniSettingsFromMemory(
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size()
        );
        return {};
    }

    lux::object::EPostStatus UISession::post(
        lux::cxx::move_only_function<void()> message
    )
    {
        return impl_->dispatcher.post(std::move(message));
    }

    ImGuiContext* UISession::imguiContext() const noexcept
    {
        return impl_->context;
    }

    void UISession::unregister(
        std::uint64_t token,
        Registration::EKind kind
    ) noexcept
    {
        if (kind == Registration::EKind::PANE)
        {
            const auto found = std::ranges::find(
                impl_->panes,
                token,
                &PaneRecord::token
            );
            if (found == impl_->panes.end()) return;
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
                [token](const FactoryRecord& record)
                {
                    return record.token == token;
                }
            );
        }
    }
}
