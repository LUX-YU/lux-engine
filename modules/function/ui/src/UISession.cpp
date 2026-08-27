#include <lux/engine/ui/UISession.hpp>

#include <algorithm>
#include <cstring>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <lux/engine/ui/detail/UiContract.hpp>

namespace lux::ui
{
    namespace detail
    {
        struct SessionControl final
        {
            explicit SessionControl(UISession* value) noexcept
                : session(value), owner(std::this_thread::get_id()), owner_token(currentUiThreadToken())
            {
            }

            UISession* session{nullptr};
            const std::thread::id owner;
            const void* const owner_token;
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
            explicit ScopedImGuiContext(ImGuiContext* target) noexcept : previous_(ImGui::GetCurrentContext())
            {
                ImGui::SetCurrentContext(target);
            }

            ~ScopedImGuiContext()
            {
                ImGui::SetCurrentContext(previous_);
            }

        private:
            ImGuiContext* previous_{nullptr};
        };

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

        struct FactoryRecord final
        {
            std::uint64_t token{0};
            PaneFactory factory;
            bool tombstone{false};
        };

    } // namespace

    struct UISession::Impl final
    {
        explicit Impl(UISession* value) noexcept : owner(value)
        {
        }

        UISession* owner{nullptr};
        ImGuiContext* context{nullptr};
        lux::object::ObjectMessageQueue messages;
        CommandRouter command_router;
        std::vector<PaneRecord> panes;
        std::vector<PaneRecord> pending_panes;
        std::vector<FactoryRecord> factories;
        std::vector<FactoryRecord> pending_factories;
        std::vector<UiContextId> focused_local_context_ids;
        std::vector<UiContextId> focused_context_ids;
        std::vector<UiContextIdView> focused_contexts;
        std::vector<UiContextIdView> frame_context_scratch;
        std::vector<UiContextIdView> frame_focused_contexts;
        PaneHandle focused_pane;
        PaneHandle hovered_pane;
        PaneHandle pending_focus;
        std::uint64_t next_token{1};
#if defined(LUX_UI_TEST_DIAGNOSTICS)
        std::uint64_t wrapper_growth_count{0};
#endif
        std::size_t factory_call_depth{0};
        bool frame_open{false};

        [[nodiscard]] static PaneHandle handle(const PaneRecord& record)
        {
            return {record.token, record.lifetime};
        }

        [[nodiscard]] static Pane* resolve(const PaneHandle& pane) noexcept
        {
            return pane ? pane.lifetime.getAsOnCurrent<Pane>() : nullptr;
        }

        [[nodiscard]] static Pane* resolve(PaneRecord& record) noexcept
        {
            if (record.tombstone)
                return nullptr;
            auto* pane = record.lifetime.getAsOnCurrent<Pane>();
            if (!pane)
                record.tombstone = true;
            return pane;
        }

        [[nodiscard]] PaneRecord* findPane(std::uint64_t token) noexcept
        {
            const auto active = std::ranges::find(panes, token, &PaneRecord::token);
            if (active != panes.end())
                return std::addressof(*active);
            const auto pending = std::ranges::find(pending_panes, token, &PaneRecord::token);
            return pending == pending_panes.end() ? nullptr : std::addressof(*pending);
        }

        [[nodiscard]] const PaneRecord* findPane(std::uint64_t token) const noexcept
        {
            const auto active = std::ranges::find(panes, token, &PaneRecord::token);
            if (active != panes.end())
                return std::addressof(*active);
            const auto pending = std::ranges::find(pending_panes, token, &PaneRecord::token);
            return pending == pending_panes.end() ? nullptr : std::addressof(*pending);
        }

        [[nodiscard]] Pane* resolveRegistered(const PaneHandle& pane) noexcept
        {
            auto* record = findPane(pane.token);
            return record ? resolve(*record) : nullptr;
        }

        [[nodiscard]] Pane* resolveRegistered(const PaneHandle& pane) const noexcept
        {
            const auto* record = findPane(pane.token);
            return record && !record->tombstone ? pane.lifetime.getAsOnCurrent<Pane>() : nullptr;
        }

        void compactPaneRecords()
        {
            const auto dead = [](const PaneRecord& record) { return record.tombstone || record.lifetime.expired(); };
            std::erase_if(panes, dead);
            std::erase_if(pending_panes, dead);
        }

        void publishPendingPanes()
        {
            compactPaneRecords();
            for (auto& record : pending_panes)
            {
                if (!record.tombstone && record.lifetime.alive())
                    panes.push_back(std::move(record));
            }
            pending_panes.clear();
        }

        void compactFactories()
        {
            std::erase_if(factories, [](const FactoryRecord& record) { return record.tombstone; });
            std::erase_if(pending_factories, [](const FactoryRecord& record) { return record.tombstone; });
        }

        void publishPendingFactories()
        {
            compactFactories();
            for (auto& record : pending_factories)
            {
                if (!record.tombstone)
                    factories.push_back(std::move(record));
            }
            pending_factories.clear();
        }

        [[nodiscard]] static bool
        sameContexts(const std::vector<UiContextId>& owned, std::span<const UiContextIdView> views) noexcept
        {
            if (owned.size() != views.size())
                return false;
            for (std::size_t index = 0; index < owned.size(); ++index)
            {
                if (owned[index].view() != views[index])
                    return false;
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
                    result.emplace_back(context.name());
            }
            return result;
        }

        void rebuildFocusedContexts()
        {
            auto* focused = resolveRegistered(focused_pane);
            if (!focused)
                focused_pane.reset();

            focused_context_ids.clear();
            focused_contexts.clear();
            const auto append_unique = [&](UiContextIdView context) {
                if (!context.isValid())
                    return;
                const auto found = std::ranges::find_if(focused_context_ids, [context](const UiContextId& value) {
                    return value.view() == context;
                }
                );
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
                    append_unique(context);
            }
            append_unique(kGlobalContext);

            focused_contexts.reserve(focused_context_ids.size());
            for (const auto& context : focused_context_ids)
                focused_contexts.push_back(context.view());
            owner->updateCommandRoute(focused, focused_contexts);
        }

        void clearFocus()
        {
            if (!focused_pane)
                return;
            auto previous = std::move(focused_pane);
            focused_pane.reset();
            focused_local_context_ids.clear();
            rebuildFocusedContexts();
            if (auto* pane = resolve(previous))
                detail::PaneStateAccess::setFocused(*pane, false);
        }

        void commitFocus(PaneHandle pane, std::span<const UiContextIdView> local_contexts)
        {
            if (pane && !resolveRegistered(pane))
                pane.reset();
            const bool pane_changed = focused_pane.token != pane.token;
            const bool contexts_changed = !sameContexts(focused_local_context_ids, local_contexts);
            if (!pane_changed && !contexts_changed)
                return;

            auto owned_contexts = ownContexts(local_contexts);
            auto previous = focused_pane;
            focused_pane = pane;
            focused_local_context_ids = std::move(owned_contexts);
            rebuildFocusedContexts();

            if (pane_changed)
            {
                if (auto* old_pane = resolve(previous))
                    detail::PaneStateAccess::setFocused(*old_pane, false);

                if (focused_pane.token != pane.token)
                    return;
                auto* new_pane = resolveRegistered(focused_pane);
                if (!new_pane)
                {
                    clearFocus();
                    return;
                }
                detail::PaneStateAccess::setFocused(*new_pane, true);
            }
        }

        void commitHover(PaneHandle pane)
        {
            if (pane && !resolveRegistered(pane))
                pane.reset();
            if (hovered_pane.token == pane.token)
                return;
            if (auto* previous = resolve(hovered_pane))
                detail::PaneStateAccess::setHovered(*previous, false);
            hovered_pane = pane;
            if (auto* current = resolveRegistered(hovered_pane))
                detail::PaneStateAccess::setHovered(*current, true);
        }
    };

    PaneRegistration::PaneRegistration(std::weak_ptr<detail::SessionControl> control, std::uint64_t token) noexcept
        : control_(std::move(control)), token_(token)
    {
    }

    PaneRegistration::PaneRegistration(PaneRegistration&& other) noexcept
        : control_(std::move(other.control_)), token_(std::exchange(other.token_, 0))
    {
    }

    PaneRegistration& PaneRegistration::operator=(PaneRegistration&& other) noexcept
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
            return;
        if (const auto control = control_.lock())
        {
            detail::requireUiOwner(control->owner, control->owner_token);
            if (control->session)
                control->session->unregisterPane(token_);
        }
        token_ = 0;
        control_.reset();
    }

    PaneFactoryRegistration::PaneFactoryRegistration(
        std::weak_ptr<detail::SessionControl> control,
        std::uint64_t token
    ) noexcept
        : control_(std::move(control)), token_(token)
    {
    }

    PaneFactoryRegistration::PaneFactoryRegistration(PaneFactoryRegistration&& other) noexcept
        : control_(std::move(other.control_)), token_(std::exchange(other.token_, 0))
    {
    }

    PaneFactoryRegistration& PaneFactoryRegistration::operator=(PaneFactoryRegistration&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            reset();
            control_ = std::move(other.control_);
            token_ = std::exchange(other.token_, 0);
        }
        return *this;
    }

    PaneFactoryRegistration::~PaneFactoryRegistration()
    {
        reset();
    }

    void PaneFactoryRegistration::reset() noexcept
    {
        if (token_ == 0)
            return;
        if (const auto control = control_.lock())
        {
            detail::requireUiOwner(control->owner, control->owner_token);
            if (control->session)
                control->session->unregisterFactory(token_);
        }
        token_ = 0;
        control_.reset();
    }

    UISession::UISession()
        : impl_(std::make_unique<Impl>(this)), control_(std::make_shared<detail::SessionControl>(this))
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
        impl_->focused_contexts.reserve(8);
        impl_->focused_local_context_ids.reserve(8);
        impl_->focused_context_ids.reserve(8);
        impl_->frame_context_scratch.reserve(8);
        impl_->frame_focused_contexts.reserve(8);
        impl_->rebuildFocusedContexts();
    }

    UISession::~UISession()
    {
        detail::requireUiOwner(control_->owner, control_->owner_token);
        control_->session = nullptr;
        impl_->clearFocus();
        impl_->commitHover({});
        impl_->messages.close();
        auto* previous = ImGui::GetCurrentContext();
        if (previous == impl_->context)
            previous = nullptr;
        ImGui::SetCurrentContext(impl_->context);
        ImGui::DestroyContext(impl_->context);
        ImGui::SetCurrentContext(previous);
    }

    void
    UISession::updateCommandRoute(lux::object::LuxObject* activation_scope, std::span<const UiContextIdView> contexts)
    {
        impl_->command_router.updateRoute(activation_scope, contexts);
    }

    lux::cxx::expected<PaneRegistration, EUiRegistrationError> UISession::registerPane(Pane& pane)
    {
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        if (!pane.id().isValid())
        {
            return lux::cxx::unexpected<EUiRegistrationError>{EUiRegistrationError::INVALID_ID};
        }
        if (!impl_->frame_open)
            impl_->compactPaneRecords();
        const auto duplicate_id = [&](const PaneRecord& record) {
            return !record.tombstone && record.lifetime.alive() && record.id.view() == pane.id().view();
        };
        if (std::ranges::any_of(impl_->panes, duplicate_id) || std::ranges::any_of(impl_->pending_panes, duplicate_id))
        {
            return lux::cxx::unexpected<EUiRegistrationError>{EUiRegistrationError::DUPLICATE_PANE_ID};
        }
        if (pane.dispatcherRef() != impl_->messages.dispatcherRef())
        {
            return lux::cxx::unexpected<EUiRegistrationError>{EUiRegistrationError::FOREIGN_SESSION};
        }
        const auto token = impl_->next_token++;
        PaneRecord record{token, pane.id(), pane.weakRef(), false};
        if (impl_->frame_open)
            impl_->pending_panes.push_back(std::move(record));
        else
            impl_->panes.push_back(std::move(record));
        return PaneRegistration{control_, token};
    }

    lux::cxx::expected<PaneFactoryRegistration, EUiRegistrationError> UISession::registerFactory(PaneFactory factory)
    {
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        if (!factory.type.isValid() || !factory.create)
        {
            return lux::cxx::unexpected<EUiRegistrationError>{EUiRegistrationError::INVALID_ID};
        }
        if (impl_->factory_call_depth == 0)
            impl_->compactFactories();
        const auto duplicate_type = [&](const FactoryRecord& record) {
            return !record.tombstone && record.factory.type == factory.type;
        };
        if (std::ranges::any_of(impl_->factories, duplicate_type) ||
            std::ranges::any_of(impl_->pending_factories, duplicate_type))
        {
            return lux::cxx::unexpected<EUiRegistrationError>{EUiRegistrationError::DUPLICATE_FACTORY};
        }
        const auto token = impl_->next_token++;
        FactoryRecord record{token, std::move(factory), false};
        if (impl_->factory_call_depth != 0)
            impl_->pending_factories.push_back(std::move(record));
        else
            impl_->factories.push_back(std::move(record));
        return PaneFactoryRegistration{control_, token};
    }

    std::unique_ptr<Pane> UISession::createPane(PaneTypeIdView type, PaneId id)
    {
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        const auto found = std::ranges::find_if(impl_->factories, [&](const FactoryRecord& record) {
            return !record.tombstone && record.factory.type.view() == type;
        }
        );
        if (found == impl_->factories.end())
            return {};

        struct FactoryCallScope final
        {
            Impl* impl;

            explicit FactoryCallScope(Impl& value) noexcept : impl(&value)
            {
                ++impl->factory_call_depth;
            }

            ~FactoryCallScope()
            {
                --impl->factory_call_depth;
                if (impl->factory_call_depth == 0)
                    impl->publishPendingFactories();
            }
        } scope{*impl_};

        return found->factory.create(impl_->messages.dispatcherRef(), std::move(id));
    }

    CommandRouter& UISession::commandRouter() noexcept
    {
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        return impl_->command_router;
    }

    const CommandRouter& UISession::commandRouter() const noexcept
    {
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        return impl_->command_router;
    }

    lux::object::ObjectDispatcherRef UISession::dispatcherRef() const noexcept
    {
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        return impl_->messages.dispatcherRef();
    }

    bool UISession::requestFocus(PaneIdView pane)
    {
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        if (!impl_->frame_open)
            impl_->compactPaneRecords();
        const auto found = std::ranges::find_if(impl_->panes, [&](const PaneRecord& record) {
            return !record.tombstone && record.id.view() == pane && record.lifetime.alive();
        }
        );
        if (found == impl_->panes.end())
            return false;
        auto* resolved = found->lifetime.getAsOnCurrent<Pane>();
        if (!resolved || !resolved->visible())
            return false;
        impl_->pending_focus = Impl::handle(*found);
        return true;
    }

    Pane* UISession::focusedPane() const noexcept
    {
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        return impl_->resolveRegistered(impl_->focused_pane);
    }

    std::span<const UiContextIdView> UISession::focusedContexts() const noexcept
    {
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        return impl_->focused_contexts;
    }

    void UISession::feedInput(const UiInputEvent& event)
    {
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        ScopedImGuiContext context{impl_->context};
        auto& io = ImGui::GetIO();
        std::visit(
            [&](const auto& value) {
                using Value = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<Value, UiPointerMove>)
                {
                    io.AddMousePosEvent(value.x, value.y);
                }
                else if constexpr (std::same_as<Value, UiPointerButton>)
                {
                    io.AddMouseButtonEvent(value.button, value.down);
                }
                else if constexpr (std::same_as<Value, UiPointerWheel>)
                {
                    io.AddMouseWheelEvent(value.horizontal, value.vertical);
                }
                else if constexpr (std::same_as<Value, UiKey>)
                {
                    if (value.key != ImGuiKey_None)
                        io.AddKeyEvent(value.key, value.down);
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
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        if (impl_->frame_open)
            detail::failUiContract();
        ScopedImGuiContext context{impl_->context};
        impl_->publishPendingPanes();
        static_cast<void>(impl_->messages.dispatchPending());
        impl_->compactPaneRecords();
        if (impl_->focused_pane && !impl_->resolveRegistered(impl_->focused_pane))
        {
            impl_->clearFocus();
        }
        if (impl_->hovered_pane && !impl_->resolveRegistered(impl_->hovered_pane))
        {
            impl_->commitHover({});
        }
        auto& io = ImGui::GetIO();
        io.DisplaySize = display_size;
        io.DeltaTime = delta_seconds;
        ImGui::NewFrame();
        impl_->frame_open = true;
    }

    void UISession::drawPanes()
    {
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        if (!impl_->frame_open)
            detail::failUiContract();
        ScopedImGuiContext context{impl_->context};
        PaneHandle focused_candidate;
        PaneHandle hovered_candidate;
        impl_->frame_focused_contexts.clear();
        for (auto& record : impl_->panes)
        {
            auto* pane = Impl::resolve(record);
            if (!pane)
                continue;
            const auto current = Impl::handle(record);
            if (!pane->visible())
            {
                if (impl_->pending_focus.token == record.token)
                    impl_->pending_focus.reset();
                continue;
            }
            if (impl_->pending_focus.token == record.token)
            {
                ImGui::SetNextWindowFocus();
                impl_->pending_focus.reset();
            }
            bool visible = true;
#if defined(LUX_UI_TEST_DIAGNOSTICS)
            const auto scratch_capacity = impl_->frame_context_scratch.capacity();
#endif
            impl_->frame_context_scratch.clear();
            PaneDrawContext draw_context{impl_->frame_context_scratch};
            if (ImGui::Begin(pane->imgui_label_.c_str(), &visible))
            {
                pane->draw(draw_context);
#if defined(LUX_UI_TEST_DIAGNOSTICS)
                impl_->wrapper_growth_count += impl_->frame_context_scratch.capacity() != scratch_capacity;
#endif
                pane = Impl::resolve(record);
                if (pane && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
                {
                    focused_candidate = current;
#if defined(LUX_UI_TEST_DIAGNOSTICS)
                    const auto focused_capacity = impl_->frame_focused_contexts.capacity();
#endif
                    impl_->frame_focused_contexts = impl_->frame_context_scratch;
#if defined(LUX_UI_TEST_DIAGNOSTICS)
                    impl_->wrapper_growth_count += impl_->frame_focused_contexts.capacity() != focused_capacity;
#endif
                }
                if (pane && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
                {
                    hovered_candidate = current;
                }
            }
            ImGui::End();
            pane = Impl::resolve(record);
            if (pane)
                pane->setVisible(visible);
        }
        impl_->commitFocus(focused_candidate, impl_->frame_focused_contexts);
        impl_->commitHover(hovered_candidate);
    }

    ImDrawData* UISession::endFrame()
    {
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        if (!impl_->frame_open)
            detail::failUiContract();
        ScopedImGuiContext context{impl_->context};
        ImGui::Render();
        impl_->frame_open = false;
        impl_->compactPaneRecords();
        return ImGui::GetDrawData();
    }

    LayoutSnapshot UISession::captureLayout() const
    {
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        ScopedImGuiContext context{impl_->context};
        std::size_t size = 0;
        const char* data = ImGui::SaveIniSettingsToMemory(&size);
        std::vector<std::byte> bytes(size);
        if (size != 0)
            std::memcpy(bytes.data(), data, size);
        return LayoutSnapshot{std::move(bytes)};
    }

    lux::cxx::expected<void, ELayoutError> UISession::restoreLayout(const LayoutSnapshot& snapshot)
    {
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        const auto bytes = snapshot.bytes();
        if (bytes.empty())
        {
            return lux::cxx::unexpected<ELayoutError>{ELayoutError::INVALID_DATA};
        }
        ScopedImGuiContext context{impl_->context};
        ImGui::LoadIniSettingsFromMemory(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return {};
    }

    void UISession::unregisterPane(std::uint64_t token) noexcept
    {
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        auto* found = impl_->findPane(token);
        if (!found || found->tombstone)
            return;
        found->tombstone = true;
        if (impl_->pending_focus.token == token)
            impl_->pending_focus.reset();
        if (impl_->focused_pane.token == token)
            impl_->clearFocus();
        if (impl_->hovered_pane.token == token)
            impl_->commitHover({});
        if (!impl_->frame_open)
            impl_->compactPaneRecords();
    }

    void UISession::unregisterFactory(std::uint64_t token) noexcept
    {
        LUX_UI_CHECK_OWNER(control_->owner, control_->owner_token);
        const auto mark = [token](std::vector<FactoryRecord>& records) {
            const auto found = std::ranges::find(records, token, &FactoryRecord::token);
            if (found != records.end())
                found->tombstone = true;
        };
        mark(impl_->factories);
        mark(impl_->pending_factories);
        if (impl_->factory_call_depth == 0)
            impl_->compactFactories();
    }

#if defined(LUX_UI_TEST_DIAGNOSTICS)
    std::uint64_t UISession::wrapperGrowthCountForTest() const noexcept
    {
        return impl_->wrapper_growth_count;
    }

    const void* UISession::contextIdentityForTest() const noexcept
    {
        return impl_->context;
    }
#endif
} // namespace lux::ui
