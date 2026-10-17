#include <lux/engine/render/data/Encoding.hpp>
#include "ServerImpl.hpp"

namespace lux::render
{
    RenderResult<StorageSet*> RenderServer::Impl::storageOwner(const VStorageOwner& address) noexcept
    {
        return std::visit([&](const auto& owner) -> RenderResult<StorageSet*> {
            using T = std::decay_t<decltype(owner)>;
            if constexpr (std::same_as<T, GlobalStorageOwner>) return &device->globalStorages();
            else if constexpr (std::same_as<T, SceneId>)
            {
                auto* scene = scenes.find(owner);
                if (!scene) return cxx::unexpected(RenderError{kRuntimeIdentity});
                return &scene->storages;
            }
            else
            {
                auto* scene = scenes.find(owner.scene);
                auto* view = scene ? scene->views.find(owner.view) : nullptr;
                if (!view) return cxx::unexpected(RenderError{kRuntimeIdentity});
                if constexpr (std::same_as<T, ViewRequest>) return &view->storages;
                else
                {
                    auto* effect = effects.find(owner.effect);
                    if (effect)
                        for (const auto& group : effect->groups)
                            if (group.scene == owner.scene && group.view == owner.view && group.target == owner.target)
                                return &group.context->attachmentStorages();
                    return cxx::unexpected(RenderError{kRuntimeIdentity});
                }
            }
        }, address);
    }

    RenderResult<void> RenderServer::Impl::findStorage(RenderRequest& request) noexcept
    {
        const auto& input = request.get<StorageLocation>();
        auto owner = storageOwner(input.owner);
        if (!owner) return cxx::unexpected(owner.error());
        if (!device->resources().factory(input.type)) return cxx::unexpected(RenderError{kStorageFactory});
        const auto id = (*owner)->find(input.type, input.key);
        if (id.isNull()) return cxx::unexpected(RenderError{kStorageIdentity});
        return request.reply(id);
    }

    RenderResult<void> RenderServer::Impl::prepareStorage(RenderRequest& request) noexcept
    {
        const auto& input = request.get<PrepareStorageRequest>();
        auto owner = storageOwner(input.location.owner);
        if (!owner) return cxx::unexpected(owner.error());
        if (request.replyCapacity() < sizeof(StorageId)) return cxx::unexpected(RenderError{kRuntimeCapacity});
        auto id = (*owner)->prepare(input.location.type, input.location.key, input.config);
        if (!id) return cxx::unexpected(id.error());
        return request.reply(*id);
    }

    RenderResult<void> RenderServer::Impl::releasePreparedStorage(RenderRequest& request) noexcept
    {
        const auto id = request.get<StorageId>();
        auto* owner = device->resources().owner(id);
        if (!owner) return cxx::unexpected(RenderError{kStorageIdentity});
        if (request.replyCapacity() < sizeof(Ack)) return cxx::unexpected(RenderError{kRuntimeCapacity});
        if (auto result = owner->releasePrepared(id); !result) return result;
        return request.reply();
    }

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
        auto* scene = scenes.find(scene_id);
        auto* view = scene ? scene->views.find(view_id) : nullptr;
        const auto* target = targets.find(target_id);
        auto* effect = effects.find(effect_id);
        const bool invalid = !view || !target || !effect;
        if (invalid) return cxx::unexpected(RenderError{kRuntimeIdentity});
        for (const auto& existing : effects)
            for (const auto& group : existing.groups)
                if (group.target == target_id) return cxx::unexpected(RenderError{kRuntimeState});
        auto prepared = device->prepare(effect->code, target->description, &scene->storages, &view->storages, effect->arguments);
        if (!prepared) return cxx::unexpected(prepared.error());
        if (!(*prepared)->contextParameters().empty())
        {
            if (auto values = (*prepared)->setContextParameters(cameraParameters(view->camera)); !values)
            {
                if (!device->release(*prepared)) std::terminate(); // unsubmitted candidate
                return cxx::unexpected(values.error());
            }
        }
        (*prepared)->setViewport(view->camera.viewport);
        AttachmentCreated response;
        (*prepared)->forEachStorage([&](std::string_view key, StorageId id) noexcept {
            response.storages.push_back({std::string(key), id});
        });
        ipc::IpcMessage output;
        auto charge = request.prepareResponse(&response, output);
        if (!charge)
        {
            if (!device->release(*prepared)) std::terminate(); // unsubmitted candidate
            return cxx::unexpected(charge.error());
        }
        effect->groups.push_back({scene_id, view_id, target_id, *prepared});
        scoped_buffers_created.store(device->resources().creations());
        return request.publishPrepared(output, *charge);
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
            // One Target writer, proven on attach. Resize builds one candidate without recompilation.
            auto candidate = device->prepare(
                std::move(code), description, &scenes.find(group->scene)->storages,
                &scenes.find(group->scene)->views.find(group->view)->storages, arguments
            );
            if (!candidate) return cxx::unexpected(candidate.error());
            if (auto values = (*candidate)->setContextParameters(group->context->contextParameters()); !values)
            {
                if (!device->release(*candidate)) std::terminate(); // unsubmitted candidate
                return cxx::unexpected(values.error());
            }
            (*candidate)->setViewport(group->context->viewport());
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
        return request.reply(SceneCreated{scenes.emplace(device->resources())});
    }

    RenderResult<void> RenderServer::Impl::removeScene(RenderRequest& request) noexcept
    {
        const auto* input = &request.get<SceneRequest>();
        const auto scene_id = input->scene;
        if (!scenes.find(scene_id)) return cxx::unexpected(RenderError{kRuntimeIdentity});
        for (auto& effect : effects)
            for (std::size_t i = effect.groups.size(); i-- > 0;)
                if (effect.groups[i].scene == scene_id)
                    if (auto result = withdraw(effect, i); !result) return result;
        scenes.erase(scene_id);
        return request.reply();
    }

    RenderResult<void> RenderServer::Impl::createView(RenderRequest& request) noexcept
    {
        const auto* input = &request.get<CreateViewRequest>();
        auto* scene = scenes.find(input->scene);
        if (!scene) return cxx::unexpected(RenderError{kRuntimeIdentity});
        if (auto valid = input->camera.validate(); !valid) return valid;
        return request.reply(ViewCreated{scene->views.emplace(device->resources(), input->camera)});
    }

    RenderResult<void> RenderServer::Impl::updateCamera(RenderRequest& request) noexcept
    {
        const auto& input = request.get<UpdateCameraRequest>();
        auto* scene = scenes.find(input.scene);
        auto* view = scene ? scene->views.find(input.view) : nullptr;
        if (!view) return cxx::unexpected(RenderError{kRuntimeIdentity});
        if (auto valid = input.camera.validate(); !valid) return valid;
        if (view->camera == input.camera) return request.reply();
        // Attach proved the complete Camera parameter ABI. This owner-thread update allocates nothing.
        for (auto& effect : effects)
            for (const auto& group : effect.groups)
            {
                if (group.scene != input.scene || group.view != input.view) continue;
                if (!group.context->contextParameters().empty() &&
                    !group.context->setContextParameters(cameraParameters(input.camera))) std::terminate();
                group.context->setViewport(input.camera.viewport);
            }
        view->camera = input.camera;
        return request.reply();
    }

    RenderResult<void> RenderServer::Impl::removeView(RenderRequest& request) noexcept
    {
        const auto* input = &request.get<ViewRequest>();
        auto* scene = scenes.find(input->scene);
        if (!scene || !scene->views.find(input->view)) return cxx::unexpected(RenderError{kRuntimeIdentity});
        for (auto& effect : effects)
            for (std::size_t i = effect.groups.size(); i-- > 0;)
                if (effect.groups[i].scene == input->scene && effect.groups[i].view == input->view)
                    if (auto result = withdraw(effect, i); !result) return result;
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
                if (auto result = withdraw(*effect, i); !result) return result;
                return request.reply();
            }
        }
        return cxx::unexpected(RenderError{kRuntimeIdentity});
    }
} // namespace lux::render
