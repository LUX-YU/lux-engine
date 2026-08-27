#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include <imgui.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/ui/CommandRouter.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/PaneFactory.hpp>
#include <lux/engine/ui/UiInputEvent.hpp>

namespace lux::ui
{
    namespace detail
    {
        struct SessionControl;
#if defined(LUX_UI_TEST_DIAGNOSTICS)
        struct UISessionDiagnosticsAccess;
#endif
    } // namespace detail

    enum class EUiRegistrationError
    {
        DUPLICATE_PANE_ID,
        DUPLICATE_FACTORY,
        INVALID_ID,
        FOREIGN_SESSION
    };

    class UISession;

    /** Live registration; reset and destruction belong to the session owner thread.
     */
    class LUX_FUNCTION_PUBLIC PaneRegistration final
    {
    public:
        PaneRegistration() noexcept = default;
        PaneRegistration(const PaneRegistration&) = delete;
        PaneRegistration& operator=(const PaneRegistration&) = delete;
        PaneRegistration(PaneRegistration&& other) noexcept;
        PaneRegistration& operator=(PaneRegistration&& other) noexcept;
        ~PaneRegistration();

        void reset() noexcept;
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return token_ != 0;
        }

    private:
        friend class UISession;
        PaneRegistration(std::weak_ptr<detail::SessionControl> control, std::uint64_t token) noexcept;

        std::weak_ptr<detail::SessionControl> control_;
        std::uint64_t token_{0};
    };

    /** Live registration; reset and destruction belong to the session owner thread.
     */
    class LUX_FUNCTION_PUBLIC PaneFactoryRegistration final
    {
    public:
        PaneFactoryRegistration() noexcept = default;
        PaneFactoryRegistration(const PaneFactoryRegistration&) = delete;
        PaneFactoryRegistration& operator=(const PaneFactoryRegistration&) = delete;
        PaneFactoryRegistration(PaneFactoryRegistration&& other) noexcept;
        PaneFactoryRegistration& operator=(PaneFactoryRegistration&& other) noexcept;
        ~PaneFactoryRegistration();

        void reset() noexcept;
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return token_ != 0;
        }

    private:
        friend class UISession;
        PaneFactoryRegistration(std::weak_ptr<detail::SessionControl> control, std::uint64_t token) noexcept;

        std::weak_ptr<detail::SessionControl> control_;
        std::uint64_t token_{0};
    };

    /**
     * Owns all live UI state and is confined to its construction thread.
     * Plain UI values such as IDs, input events and LayoutSnapshot remain
     * ordinary transferable values.
     */
    class LUX_FUNCTION_PUBLIC UISession final
    {
    public:
        UISession();
        ~UISession();
        UISession(const UISession&) = delete;
        UISession& operator=(const UISession&) = delete;

        [[nodiscard]] lux::cxx::expected<PaneRegistration, EUiRegistrationError> registerPane(Pane& pane);

        [[nodiscard]] lux::cxx::expected<PaneFactoryRegistration, EUiRegistrationError>
        registerFactory(PaneFactory factory);

        [[nodiscard]] std::unique_ptr<Pane> createPane(PaneTypeIdView type, PaneId id);

        [[nodiscard]] CommandRouter& commandRouter() noexcept;

        [[nodiscard]] const CommandRouter& commandRouter() const noexcept;

        [[nodiscard]] lux::object::ObjectDispatcherRef dispatcherRef() const noexcept;

        /** Returns whether the focus request was accepted, not canonical focus. */
        [[nodiscard]] bool requestFocus(PaneIdView pane);
        [[nodiscard]] Pane* focusedPane() const noexcept;
        [[nodiscard]] std::span<const UiContextIdView> focusedContexts() const noexcept;

        void beginFrame(ImVec2 display_size, float delta_seconds);
        void feedInput(const UiInputEvent& event);
        void drawPanes();
        [[nodiscard]] ImDrawData* endFrame();

        [[nodiscard]] LayoutSnapshot captureLayout() const;

        [[nodiscard]] lux::cxx::expected<void, ELayoutError> restoreLayout(const LayoutSnapshot& snapshot);

    private:
        friend class PaneRegistration;
        friend class PaneFactoryRegistration;
#if defined(LUX_UI_TEST_DIAGNOSTICS)
        friend struct detail::UISessionDiagnosticsAccess;
#endif
        void updateCommandRoute(lux::object::LuxObject* activation_scope, std::span<const UiContextIdView> contexts);
        void unregisterPane(std::uint64_t token) noexcept;
        void unregisterFactory(std::uint64_t token) noexcept;
#if defined(LUX_UI_TEST_DIAGNOSTICS)
        [[nodiscard]] std::uint64_t wrapperGrowthCountForTest() const noexcept;
        [[nodiscard]] const void* contextIdentityForTest() const noexcept;
#endif

        struct Impl;
        std::unique_ptr<Impl> impl_;
        std::shared_ptr<detail::SessionControl> control_;
    };
} // namespace lux::ui
