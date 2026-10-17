#include <lux/engine/render/protocol/AsyncCommand.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialCommands.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>

namespace lux::render::vulkan
{
    RenderResult<void> MaterialCommands::set(const SetMaterial::Request&, Reply<SetMaterial>& reply) noexcept
    {
        auto pending = reply.defer();
        if (!pending) return cxx::unexpected(pending.error());
        auto storage = device_.resources().borrow<MaterialStorage>(pending->request().material.storage);
        if (!storage) return pending->fail(storage.error());
        auto candidate = render::upload(**storage, pending->request());
        if (!candidate) return pending->fail(candidate.error());
        return pending->completeWhen(GpuWorkSender<MaterialPublished>{
            std::move(candidate->first), std::move(candidate->second)
        });
    }

    RenderResult<void> MaterialCommands::query(const QueryMaterial::Request& input, Reply<QueryMaterial>& reply) noexcept
    {
        auto storage = device_.resources().tryGet<render::MaterialStorage>(input.storage);
        if (!storage) return cxx::unexpected(storage.error());
        auto result = storage->query(input);
        if (!result) return cxx::unexpected(result.error());
        return reply.complete(std::move(*result));
    }

    RenderResult<void> MaterialCommands::release(const ReleaseMaterial::Request& input, Reply<ReleaseMaterial>& reply) noexcept
    {
        auto storage = device_.resources().borrow<MaterialStorage>(input.material.storage);
        if (!storage) return cxx::unexpected(storage.error());
        if (auto result = (*storage)->release(input.material, input.revision); !result) return result;
        return reply.complete(Ack{});
    }

    StorageFactory StorageProvider<render::MaterialStorage>::factory() noexcept { return materialStorageFactory(); }

    StorageCppType StorageProvider<render::MaterialStorage>::implementationType() noexcept
    {
        return storageCppType<Storage>();
    }
}
