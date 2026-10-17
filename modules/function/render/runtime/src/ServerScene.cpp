#include "ServerImpl.hpp"

namespace lux::render
{
    RenderResult<void> RenderServer::Impl::withdraw(RenderEffect& effect, std::size_t index) noexcept
    {
        auto* context = effect.groups[index].context;
        if (auto result = device->release(context); !result) return result;
        effect.groups.erase(effect.groups.begin() + index);
        return {};
    }

    RenderResult<void> RenderServer::Impl::attach(RenderRequest& request) noexcept
    {
        const auto* decoded = &request.get<AttachmentRequest>();
        const auto [scene_id, view_id, target_id, effect_id] = *decoded;
        const auto* scene = scenes.find(scene_id);
        const auto* view = scene ? scene->views.find(view_id) : nullptr;
        const auto* target = targets.find(target_id);
        auto* effect = effects.find(effect_id);
        const bool invalid = !view || !target || !effect;
        if (invalid) return cxx::unexpected(RenderError{kRuntimeIdentity});
        for (const auto& existing : effects)
            for (const auto& group : existing.groups)
                if (group.target == target_id) return cxx::unexpected(RenderError{kRuntimeState});
        auto prepared = device->prepare(effect->code, target->description, view->domain, effect->arguments);
        if (!prepared) return cxx::unexpected(prepared.error());
        (*prepared)->setColor(view->color);
        effect->groups.push_back({scene_id, view_id, target_id, *prepared});
        scoped_buffers_created.store(device->resources().creations());
        return request.reply();
    }

    RenderResult<void> RenderServer::Impl::resize(TargetId id, Target& target, const TargetDesc& description) noexcept
    {
        ExecutionGroup* group = nullptr;
        std::shared_ptr<const vulkan::CompiledGraph> code;
        std::span<const StorageArgument> arguments;
        for (auto& effect : effects)
            for (auto& candidate : effect.groups)
                if (candidate.target == id)
                {
                    group = &candidate;
                    code = effect.code;
                    arguments = effect.arguments;
                }
        if (group)
        {
            if (auto admitted = admitWithdraw([&](const auto&, const auto& current) { return current.target == id; });
                !admitted)
                return admitted;
            // One Target writer, proven on attach. Resize builds one candidate without recompilation.
            auto candidate = device->prepare(
                std::move(code), description, scenes.find(group->scene)->views.find(group->view)->domain, arguments
            );
            if (!candidate) return cxx::unexpected(candidate.error());
            (*candidate)->setColor(group->context->color());
            if (auto result = device->release(group->context); !result)
            {
                if (!device->release(*candidate)) std::terminate(); // never submitted
                return result;
            }
            group->context = *candidate;
        }
        target.description = description;
        ++target.epoch;
        scoped_buffers_created.store(device->resources().creations());
        return {};
    }

    RenderResult<void> RenderServer::Impl::createScene(RenderRequest& request) noexcept
    {
        auto domain = device->resources().createDomain(device->resources().root(), EResourceScope::SCENE);
        if (!domain) return cxx::unexpected(domain.error());
        return request.reply(SceneCreated{scenes.emplace(Scene{std::move(*domain), {}})});
    }

    RenderResult<void> RenderServer::Impl::removeScene(RenderRequest& request) noexcept
    {
        const auto* input = &request.get<SceneRequest>();
        const auto scene_id = input->scene;
        if (!scenes.find(scene_id)) return cxx::unexpected(RenderError{kRuntimeIdentity});
        if (auto admitted = admitWithdraw([&](const auto&, const auto& group) { return group.scene == scene_id; });
            !admitted)
            return admitted;
        for (auto& effect : effects)
            for (std::size_t i = effect.groups.size(); i-- > 0;)
                if (effect.groups[i].scene == scene_id)
                    if (auto result = withdraw(effect, i); !result) return result;
        (void)device->resources().closeDomain(scenes.find(scene_id)->domain);
        scenes.erase(scene_id);
        return request.reply();
    }

    RenderResult<void> RenderServer::Impl::createView(RenderRequest& request) noexcept
    {
        const auto* input = &request.get<CreateViewRequest>();
        auto* scene = scenes.find(input->scene);
        if (!scene) return cxx::unexpected(RenderError{kRuntimeIdentity});
        if (!std::ranges::all_of(input->color, [](float x) { return std::isfinite(x); }))
            return cxx::unexpected(RenderError{kInvalidEncoding});
        auto domain = device->resources().createDomain(scene->domain, EResourceScope::VIEW);
        if (!domain) return cxx::unexpected(domain.error());
        return request.reply(ViewCreated{scene->views.emplace(View{input->color, std::move(*domain)})});
    }

    RenderResult<void> RenderServer::Impl::removeView(RenderRequest& request) noexcept
    {
        const auto* input = &request.get<ViewRequest>();
        auto* scene = scenes.find(input->scene);
        if (!scene || !scene->views.find(input->view)) return cxx::unexpected(RenderError{kRuntimeIdentity});
        if (auto admitted = admitWithdraw([&](const auto&, const auto& group)
                                          { return group.scene == input->scene && group.view == input->view; });
            !admitted)
            return admitted;
        for (auto& effect : effects)
            for (std::size_t i = effect.groups.size(); i-- > 0;)
                if (effect.groups[i].scene == input->scene && effect.groups[i].view == input->view)
                    if (auto result = withdraw(effect, i); !result) return result;
        (void)device->resources().closeDomain(scene->views.find(input->view)->domain);
        scene->views.erase(input->view);
        return request.reply();
    }

    RenderResult<void> RenderServer::Impl::createTarget(RenderRequest& request) noexcept
    {
        const auto* input = &request.get<CreateTargetRequest>();
        if (auto valid = input->description.validate(); !valid) return valid;
        if (input->description.kind != ETargetKind::OFFSCREEN) return cxx::unexpected(RenderError{kRuntimeUnsupported});
        return request.reply(TargetCreated{targets.emplace(Target{input->description}), 1});
    }

    RenderResult<void> RenderServer::Impl::resizeTarget(RenderRequest& request) noexcept
    {
        const auto* input = &request.get<ResizeTargetRequest>();
        if (auto valid = input->description.validate(); !valid) return valid;
        if (input->description.kind != ETargetKind::OFFSCREEN) return cxx::unexpected(RenderError{kRuntimeUnsupported});
        auto* target = targets.find(input->target);
        if (!target) return cxx::unexpected(RenderError{kRuntimeIdentity});
        if (target->epoch == UINT64_MAX) return cxx::unexpected(RenderError{kRuntimeCapacity});
        if (auto changed = resize(input->target, *target, input->description); !changed) return changed;
        return request.reply(TargetCreated{input->target, target->epoch});
    }

    RenderResult<void> RenderServer::Impl::detach(RenderRequest& request) noexcept
    {
        const auto* input = &request.get<AttachmentRequest>();
        auto* effect = effects.find(input->effect);
        if (!effect) return cxx::unexpected(RenderError{kRuntimeIdentity});
        for (std::size_t i = 0; i < effect->groups.size(); ++i)
        {
            const auto& group = effect->groups[i];
            if (group.scene == input->scene && group.view == input->view && group.target == input->target)
            {
                if (auto admitted = admitWithdraw([&](const auto&, const auto& current)
                                                  { return current.context == group.context; });
                    !admitted)
                    return admitted;
                if (auto result = withdraw(*effect, i); !result) return result;
                return request.reply();
            }
        }
        return cxx::unexpected(RenderError{kRuntimeIdentity});
    }
} // namespace lux::render
