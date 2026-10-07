#include <lux/engine/ui/detail/RootImpl.hpp>

namespace lux::ui
{
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
        data->dock_state.enabled = config.docking;
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
    cxx::expected<void, ECaptureError> Root::update() noexcept
    {
        return updateFrame({}, nullptr, std::nullopt);
    }
    cxx::expected<void, ECaptureError> Root::update(
        FrameInfo info,
        DrawData& output,
        std::optional<Capture> capture
    ) noexcept
    {
        return updateFrame(info, &output, capture);
    }
    lux::cxx::expected<void, ECaptureError> Root::updateFrame(
        FrameInfo info,
        DrawData* output,
        std::optional<Capture> capture
    ) noexcept
    {
        requireOwner();
        const bool is_active_visit = impl_->drawing || impl_->updating || impl_->layout_state.depth != 0;
        const bool is_active_callback =
            impl_->change_state.batch_size != 0 || impl_->change_state.active || isDispatching();
        if (is_active_visit || is_active_callback)
        {
            return lux::cxx::unexpected(ECaptureError::FRAME_OPEN);
        }
        applyPendingChanges();
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
        ++impl_->layout_state.epoch;
        impl_->focus_state.draw_focused = impl_->focus_state.draw_hovered = nullptr;
        impl_->focus_state.draw_focused_element = impl_->focus_state.draw_hovered_element = nullptr;
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
        impl_->focus_state.focused_element = impl_->focus_state.draw_focused_element;
        impl_->focus_state.hovered_element = impl_->focus_state.draw_hovered_element;
        auto* old_focus = impl_->focus_state.focused;
        auto* old_hover = impl_->focus_state.hovered;
        if (old_focus != impl_->focus_state.draw_focused)
        {
            impl_->focus_state.focused = impl_->focus_state.draw_focused;
            if (old_focus)
            {
                old_focus->setFocused(false);
            }
            if (impl_->focus_state.draw_focused)
            {
                impl_->focus_state.draw_focused->setFocused(true);
            }
        }
        if (old_hover)
        {
            old_hover->setHovered(false);
        }
        impl_->focus_state.hovered = impl_->focus_state.draw_hovered;
        if (impl_->focus_state.draw_hovered)
        {
            impl_->focus_state.draw_hovered->setHovered(true);
        }
        return impl_->context->endFrame(output);
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
        if (const auto capture = impl_->focus_state.pointer_capture;
            capture && (!capture.visible() || !allowedByModal(*capture.containingPane())))
        {
            impl_->focus_state.pointer_capture = {};
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

} // namespace lux::ui
