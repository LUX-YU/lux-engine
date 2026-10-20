#include <lux/engine/render/runtime/ServerImpl.hpp>

namespace lux::render
{
    RenderResult<StorageSet*> RenderServer::Impl::findStorageSet(const VStorageLocation& address) noexcept
    {
        return std::visit(
            [&](const auto& location) -> RenderResult<StorageSet*>
            {
                using T = std::decay_t<decltype(location)>;
                if constexpr (std::same_as<T, GlobalStorageLocation>)
                {
                    return &device->globalStorages();
                }
                else if constexpr (std::same_as<T, SceneId>)
                {
                    auto* scene = scenes.find(location);
                    if (!scene)
                    {
                        return cxx::unexpected(RenderError{kRuntimeIdentity});
                    }
                    return &scene->storages;
                }
                else if constexpr (std::same_as<T, ViewRequest>)
                {
                    auto* scene = scenes.find(location.scene);
                    auto* view = scene ? scene->views.find(location.view) : nullptr;
                    if (!view)
                    {
                        return cxx::unexpected(RenderError{kRuntimeIdentity});
                    }
                    return &view->storages;
                }
                else
                {
                    auto* attachment = attachments.find(location);
                    if (!attachment)
                    {
                        return cxx::unexpected(RenderError{kRuntimeIdentity});
                    }
                    return &attachment->context->attachmentStorages();
                }
            },
            address
        );
    }

    void RenderServer::Impl::findStorage(
        const FindStorage::Request& _request,
        FindStorage::Response& output_response
    ) noexcept
    {
        const auto& input = _request;
        auto storage_set = findStorageSet(input.location);
        if (!storage_set)
        {
            output_response.error = storage_set.error();
            return;
        }
        if (!device->resources().factory(input.type))
        {
            output_response.error = RenderError{kStorageFactory};
            return;
        }
        const auto id = (*storage_set)->find(input.type, input.key);
        if (id.isNull())
        {
            output_response.error = RenderError{kStorageIdentity};
            return;
        }
        output_response = {{}, id};
    }

    void RenderServer::Impl::prepareStorage(
        const PrepareStorage::Request& _request,
        PrepareStorage::Response& output_response
    ) noexcept
    {
        const auto& input = _request;
        auto storage_set = findStorageSet(input.location.location);
        if (!storage_set)
        {
            output_response.error = storage_set.error();
            return;
        }
        auto id = (*storage_set)->getOrCreate(input.location.type, input.location.key, input.config);
        if (!id)
        {
            output_response.error = id.error();
            return;
        }
        output_response = {{}, *id};
    }

    void RenderServer::Impl::removeStorage(
        const RemoveStorage::Request& _request,
        RemoveStorage::Response& output_response
    ) noexcept
    {
        const auto id = _request;
        auto* storage_set = device->resources().storageSet(id);
        if (!storage_set)
        {
            output_response.error = RenderError{kStorageIdentity};
            return;
        }
        if (auto result = storage_set->remove(id); !result)
        {
            output_response.error = result.error();
            return;
        }
        output_response = {};
    }

    void RenderServer::Impl::detachAttachment(AttachmentId id) noexcept
    {
        auto& attachment = *attachments.find(id);
        attachment.context.reset();
        auto& view = *scenes.find(attachment.scene)->views.find(attachment.view);
        auto& effect = *effects.find(attachment.effect);
        auto& target = *targets.find(attachment.target);
        const auto last_view = view.attachments.back();
        view.attachments[attachment.view_position] = last_view;
        attachments.find(last_view)->view_position = attachment.view_position;
        view.attachments.pop_back();

        const auto last_effect = effect.attachments.back();
        effect.attachments[attachment.effect_position] = last_effect;
        attachments.find(last_effect)->effect_position = attachment.effect_position;
        effect.attachments.pop_back();
        target.attachment = {};
        active_targets.erase(attachment.target);
        attachments.erase(id);
    }

    void RenderServer::Impl::attachEffect(const Attach::Request& _request, Attach::Response& output_response) noexcept
    {

        const auto& scene_id = _request.scene;
        const auto& view_id = _request.view;
        const auto& target_id = _request.target;
        const auto& effect_id = _request.effect;
        auto* scene = scenes.find(scene_id);
        auto* view = scene ? scene->views.find(view_id) : nullptr;
        auto* target = targets.find(target_id);
        auto* effect = effects.find(effect_id);
        const bool invalid = !view || !target || !effect;
        if (invalid)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (!target->attachment.isNull())
        {
            output_response.error = RenderError{kRuntimeState};
            return;
        }
        if (auto valid = _request.viewport.validate(); !valid)
        {
            output_response.error = valid.error();
            return;
        }
        auto prepared =
            device->prepare(*effect->code, *target->physical, &scene->storages, &view->storages, effect->arguments);
        if (!prepared)
        {
            output_response.error = prepared.error();
            return;
        }
        if (auto values = (*prepared)->setContextParameters(_request.parameters); !values)
        {
            output_response.error = values.error();
            return;
        }
        (*prepared)->setViewport(_request.viewport);
        output_response = {};
        (*prepared)->forEachStorage([&](std::string_view key, StorageId id) noexcept
                                    { output_response.storages.push_back({std::string(key), id}); });
        const auto id =
            attachments.emplace(EffectAttachment{effect_id, scene_id, view_id, target_id, std::move(*prepared)});
        auto& attachment = *attachments.find(id);
        attachment.view_position = view->attachments.size();
        attachment.effect_position = effect->attachments.size();
        view->attachments.push_back(id);
        effect->attachments.push_back(id);
        target->attachment = id;
        output_response.attachment = id;
        active_targets.emplace(target_id);
        storages_created.store(device->resources().creations());
        return;
    }

    RenderResult<void> RenderServer::Impl::resizeTargetResources(
        RenderTarget& target,
        const TargetDesc& description
    ) noexcept
    {
        auto physical = detail::OffscreenTarget::create(*device, description);
        if (!physical)
        {
            return cxx::unexpected(physical.error());
        }
        auto* group = target.attachment.isNull() ? nullptr : attachments.find(target.attachment);
        if (group)
        {
            // One Target writer, proven on attach. Resize builds one candidate without recompilation.
            const auto& effect = *effects.find(group->effect);
            auto candidate = device->prepare(
                *effect.code,
                **physical,
                &scenes.find(group->scene)->storages,
                &scenes.find(group->scene)->views.find(group->view)->storages,
                effect.arguments
            );
            if (!candidate)
            {
                return cxx::unexpected(candidate.error());
            }
            if (auto values = (*candidate)->setContextParameters(group->context->contextParameters()); !values)
            {
                return cxx::unexpected(values.error());
            }
            (*candidate)->setViewport(group->context->viewport());
            group->context = std::move(*candidate);
        }
        target.physical = std::move(*physical);
        target.description = description;
        ++target.epoch;
        storages_created.store(device->resources().creations());
        return {};
    }

    void RenderServer::Impl::createScene(
        const CreateScene::Request& _request,
        CreateScene::Response& output_response
    ) noexcept
    {
        output_response = SceneCreated{{}, scenes.emplace(device->resources())};
    }

    void RenderServer::Impl::removeScene(
        const RemoveScene::Request& _request,
        RemoveScene::Response& output_response
    ) noexcept
    {
        const auto* input = &_request;
        const auto scene_id = input->scene;
        auto* scene = scenes.find(scene_id);
        if (!scene)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        for (auto& view : scene->views)
        {
            while (!view.attachments.empty())
            {
                detachAttachment(view.attachments.back());
            }
        }
        scenes.erase(scene_id);
        output_response = {};
    }

    void RenderServer::Impl::createView(
        const CreateView::Request& _request,
        CreateView::Response& output_response
    ) noexcept
    {
        const auto* input = &_request;
        auto* scene = scenes.find(input->scene);
        if (!scene)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        output_response = ViewCreated{{}, scene->views.emplace(device->resources())};
    }

    void RenderServer::Impl::updateAttachmentInputs(
        const UpdateAttachmentInputs::Request& input,
        UpdateAttachmentInputs::Response& output_response
    ) noexcept
    {
        auto* attachment = attachments.find(input.attachment);
        if (!attachment)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (auto valid = input.viewport.validate(); !valid)
        {
            output_response.error = valid.error();
            return;
        }
        // Native validates the complete input size before changing any parameter bytes.
        if (auto applied = attachment->context->setContextParameters(input.parameters); !applied)
        {
            output_response.error = applied.error();
            return;
        }
        attachment->context->setViewport(input.viewport);
        output_response = {};
    }

    void RenderServer::Impl::removeView(
        const RemoveView::Request& _request,
        RemoveView::Response& output_response
    ) noexcept
    {
        const auto* input = &_request;
        auto* scene = scenes.find(input->scene);
        auto* view = scene ? scene->views.find(input->view) : nullptr;
        if (!view)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        while (!view->attachments.empty())
        {
            detachAttachment(view->attachments.back());
        }
        scene->views.erase(input->view);
        output_response = {};
    }

    void RenderServer::Impl::createTarget(
        const CreateTarget::Request& _request,
        CreateTarget::Response& output_response
    ) noexcept
    {
        const auto* input = &_request;
        if (auto valid = input->description.validate(); !valid)
        {
            output_response.error = valid.error();
            return;
        }
        if (input->description.kind != ETargetKind::OFFSCREEN)
        {
            output_response.error = RenderError{kRuntimeUnsupported};
            return;
        }
        auto physical = detail::OffscreenTarget::create(*device, input->description);
        if (!physical)
        {
            output_response.error = physical.error();
            return;
        }
        output_response = TargetCreated{{}, targets.emplace(std::move(*physical)), 1};
    }

    void RenderServer::Impl::resizeTarget(
        const ResizeTarget::Request& _request,
        ResizeTarget::Response& output_response
    ) noexcept
    {
        const auto* input = &_request;
        if (auto valid = input->description.validate(); !valid)
        {
            output_response.error = valid.error();
            return;
        }
        if (input->description.kind != ETargetKind::OFFSCREEN)
        {
            output_response.error = RenderError{kRuntimeUnsupported};
            return;
        }
        auto* target = targets.find(input->target);
        if (!target)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (target->epoch == UINT64_MAX)
        {
            output_response.error = RenderError{kRuntimeCapacity};
            return;
        }
        if (auto changed = resizeTargetResources(*target, input->description); !changed)
        {
            output_response.error = changed.error();
            return;
        }
        output_response = TargetCreated{{}, input->target, target->epoch};
    }

    void RenderServer::Impl::detachEffect(const Detach::Request& _request, Detach::Response& output_response) noexcept
    {
        const auto* attachment = attachments.find(_request);
        if (!attachment)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        detachAttachment(_request);
        output_response = {};
    }
} // namespace lux::render
