#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include <imgui.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/ui_next/CommandRouter.hpp>
#include <lux/engine/ui_next/Layout.hpp>
#include <lux/engine/ui_next/PaneFactory.hpp>
#include <lux/engine/ui_next/UiInputEvent.hpp>

namespace lux::ui
{
    namespace detail
    {
        struct SessionControl;
    }

    enum class EUiRegistrationError
    {
        DUPLICATE_PANE_ID,
        DUPLICATE_FACTORY,
        INVALID_ID
    };

    class LUX_FUNCTION_PUBLIC UISession final
    {
    public:
        class LUX_FUNCTION_PUBLIC Registration
        {
        public:
            Registration() noexcept = default;
            Registration(const Registration&) = delete;
            Registration& operator=(const Registration&) = delete;
            Registration(Registration&& other) noexcept;
            Registration& operator=(Registration&& other) noexcept;
            ~Registration();

            void reset() noexcept;
            [[nodiscard]] explicit operator bool() const noexcept
            {
                return token_ != 0;
            }

        private:
            friend class UISession;
            enum class EKind
            {
                PANE,
                FACTORY
            };
            Registration(
                std::weak_ptr<detail::SessionControl> control,
                std::uint64_t token,
                EKind kind
            ) noexcept;

            std::weak_ptr<detail::SessionControl> control_;
            std::uint64_t token_{0};
            EKind kind_{EKind::PANE};
        };

        UISession();
        ~UISession();
        UISession(const UISession&) = delete;
        UISession& operator=(const UISession&) = delete;

        [[nodiscard]] lux::cxx::expected<Registration, EUiRegistrationError>
        registerPane(Pane& pane);

        [[nodiscard]] lux::cxx::expected<Registration, EUiRegistrationError>
        registerFactory(PaneFactory factory);

        [[nodiscard]] std::unique_ptr<Pane> createPane(PaneTypeIdView type, PaneId id);

        [[nodiscard]] CommandRouter& commandRouter() noexcept;
        [[nodiscard]] const CommandRouter& commandRouter() const noexcept;
        [[nodiscard]] lux::object::ObjectDispatcherRef dispatcherRef() const noexcept;

        [[nodiscard]] bool focusPane(PaneIdView pane);
        [[nodiscard]] Pane* focusedPane() const noexcept;
        [[nodiscard]] std::span<const UiContextIdView> focusedContexts() const noexcept;

        void beginFrame(ImVec2 display_size, float delta_seconds);
        void feedInput(const UiInputEvent& event);
        void drawPanes();
        [[nodiscard]] ImDrawData* endFrame();

        [[nodiscard]] LayoutSnapshot captureLayout() const;
        [[nodiscard]] lux::cxx::expected<void, ELayoutError>
        restoreLayout(std::span<const std::byte> bytes);

        [[nodiscard]] ImGuiContext* imguiContext() const noexcept;

    private:
        friend class Registration;
        void unregister(std::uint64_t token, Registration::EKind kind) noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;
        std::shared_ptr<detail::SessionControl> control_;
    };
} // namespace lux::ui
