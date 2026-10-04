#include <exception>
#include <lux/engine/editor/scene/ResourceView.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/editor/scene/SceneView.hpp>
#include <lux/engine/editor/desktop/ViewHost.hpp>
#include <imgui.h>

namespace lux::editor::scene
{
    struct ResourceView::Impl final
    {
        struct Viewport final
        {
            desktop::ViewHost& host;
            views::ViewId id;
        };
        std::optional<Viewport> viewport_;
        views::ViewContent association_;
        lux::scene::SceneRuntime& runtime_;
        std::optional<ResourceViewBinding> binding_;
        ResourceStatusSnapshot snapshot_;
        std::vector<std::string> labels_;
        render::RenderResult<void> status_;
        std::optional<ResourceRetryRequest> retry_;
        struct Content final : lux::ui::Element
        {
            Impl& owner_;
            Content(ResourceView& view, Impl& owner) : Element(view, lux::ui::ElementId{"resources"}), owner_(owner)
            {
                setStretch({1, 1});
            }
            void draw() noexcept override
            {
                if (!owner_.binding_)
                    ImGui::TextUnformatted("No scene bound");
                if (!owner_.status_)
                    ImGui::Text("Resource query/retry error: %u", static_cast<unsigned>(owner_.status_.error().code));
                constexpr const char*
                    states[]{"Unreferenced", "Reading", "Uploading", "Ready", "Failed", "Cancelled", "Capacity"};
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(owner_.snapshot_.rows.size()));
                while (clipper.Step())
                    for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
                    {
                        const auto& row = owner_.snapshot_.rows[index];
                        ImGui::PushID(index);
                        ImGui::Text("%s: %s", owner_.labels_[index].c_str(), states[static_cast<unsigned>(row.state)]);
                        if (row.state == lux::scene::ERenderAssetState::FAILED)
                        {
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Retry"))
                                owner_.retry_ = {owner_.snapshot_.instance, owner_.snapshot_.system, row.key};
                        }
                        ImGui::PopID();
                    }
            }
        } content_;
        Impl(ResourceView& view, lux::scene::SceneRuntime& runtime) : runtime_(runtime), content_(view, *this) {}
        void install(ResourceStatusSnapshot candidate)
        {
            labels_.clear();
            labels_.reserve(candidate.rows.size());
            for (const auto& row : candidate.rows)
                labels_.push_back(uuids::to_string(row.key.mesh.uuid()));
            snapshot_ = std::move(candidate);
        }
        render::RenderResult<void> rebind(std::optional<ResourceViewBinding> binding)
        {
            if (binding == binding_)
                return {};
            ResourceStatusSnapshot candidate;
            if (binding)
            {
                auto captured = captureResourceStatus(runtime_, binding->instance, binding->system);
                if (!captured)
                    return cxx::unexpected(captured.error());
                candidate = std::move(*captured);
            }
            binding_ = binding;
            install(std::move(candidate));
            retry_.reset();
            return {};
        }
        render::RenderResult<void> synchronizeViewport(const Viewport& source)
        {
            std::optional<ResourceViewBinding> target;
            views::ViewContent content;
            bool is_scene{};
            const auto read = [&](lux::ui::Pane& pane) {
                if (const auto* view = dynamic_cast<const SceneView*>(&pane))
                {
                    is_scene = true;
                    if (view->presentedInstance().valid())
                        target = ResourceViewBinding{view->presentedInstance(), view->renderSystem()};
                    if (const auto* author = std::get_if<EditedSceneBinding>(&view->binding()))
                        content = {{author->session.id()}, author->session.id()};
                }
            };
            auto observed = source.host.withView(source.id, read);
            if (!observed && observed.error() != views::EViewError::INVALID_ID)
                return cxx::unexpected(render::RendererFailure{
                    observed.error() == views::EViewError::BUSY ? render::ERendererError::BUSY
                                                               : render::ERendererError::INVALID_ARGUMENT
                });
            if (observed && !is_scene)
                return cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
            auto changed = rebind(target);
            if (changed)
                association_ = std::move(content);
            return changed;
        }
        render::RenderResult<void> refresh()
        {
            if (!binding_)
                return {};
            // No row allocation when the actual RenderAssets revision is unchanged.
            auto registry = std::as_const(runtime_).borrowInstance(binding_->instance);
            if (!registry)
            {
                const auto* error = std::get_if<lux::scene::ESceneRuntimeError>(&registry.error().cause);
                return cxx::unexpected(render::RendererFailure{
                    error && *error == lux::scene::ESceneRuntimeError::BUSY ? render::ERendererError::BUSY
                                                                            : render::ERendererError::INVALID_ARGUMENT
                });
            }
            const auto* assets = lux::scene::RenderAssets::find(registry->get(), binding_->system);
            if (!assets)
                return cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
            if (assets->revision() != snapshot_.revision)
            {
                auto candidate = captureResourceStatus(runtime_, binding_->instance, binding_->system);
                if (!candidate)
                    return cxx::unexpected(candidate.error());
                install(std::move(*candidate));
            }
            return {};
        }
    };
    ResourceView::ResourceView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        lux::scene::SceneRuntime& runtime
    )
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.resources"}, "Resources"),
          impl_(std::make_unique<Impl>(*this, runtime))
    {
        if (!setContent(impl_->content_))
            std::terminate(); // Fixed content in a detached Pane.
    }
    ResourceView::~ResourceView() noexcept = default;
    render::RenderResult<void> ResourceView::rebind(std::optional<ResourceViewBinding> binding)
    {
        return impl_->status_ = impl_->rebind(binding);
    }
    render::RenderResult<void> ResourceView::followViewport(desktop::ViewHost& host, views::ViewId id)
    {
        const Impl::Viewport source{host, id};
        auto changed = impl_->synchronizeViewport(source);
        if (changed)
            impl_->viewport_.emplace(source);
        return changed;
    }
    views::ViewContent ResourceView::content() const noexcept
    {
        return impl_->association_;
    }
    render::RenderResult<void> ResourceView::refresh()
    {
        return impl_->status_ = impl_->refresh();
    }
    render::RenderResult<void> ResourceView::retry(const lux::scene::RenderAssetKey& key)
    {
        if (!impl_->binding_)
            return cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        return impl_->status_ =
                   retryResource(impl_->runtime_, {impl_->binding_->instance, impl_->binding_->system, key});
    }
    const ResourceStatusSnapshot& ResourceView::snapshot() const noexcept
    {
        return impl_->snapshot_;
    }
    const render::RenderResult<void>& ResourceView::status() const noexcept
    {
        return impl_->status_;
    }
    void ResourceView::update() noexcept
    {
        if (impl_->viewport_)
        {
            impl_->status_ = impl_->synchronizeViewport(*impl_->viewport_);
            if (!impl_->status_)
                return;
        }
        static_cast<void>(refresh());
        if (impl_->retry_)
        {
            impl_->status_ = retryResource(impl_->runtime_, *impl_->retry_);
            // A UI retry is an admission request. Its error remains visible; accepted asynchronous
            // completion belongs to RenderAssets, never to this window.
            if (impl_->status_ || impl_->status_.error().code != render::ERendererError::BUSY)
                impl_->retry_.reset();
        }
    }
    render::RenderResult<views::DetachedView> makeResourceView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        lux::scene::SceneRuntime& runtime,
        std::optional<ResourceViewBinding> binding
    )
    {
        auto result = std::make_unique<ResourceView>(dispatcher, std::move(id), runtime);
        auto bound = result->rebind(binding);
        if (!bound)
            return cxx::unexpected(bound.error());
        return views::DetachedView{
            lux::object::CodeLease::builtin(), std::move(result), nullptr, nullptr, nullptr, nullptr,
            +[](const lux::ui::Pane& pane) noexcept { return static_cast<const ResourceView&>(pane).content(); }
        };
    }
}
