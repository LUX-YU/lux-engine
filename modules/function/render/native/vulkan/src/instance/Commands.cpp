#include <lux/engine/render/native/vulkan/instance/InstanceCommands.hpp>
#include <lux/engine/render/native/vulkan/instance/InstanceStorage.hpp>

namespace lux::render::vulkan
{
    RenderResult<void> InstanceCommands::create(const InstanceCreate& input, Reply<CreateInstance>& reply) noexcept
    {
        auto storage = device_.resources().borrow<InstanceStorage>(input.storage);
        if (!storage) return cxx::unexpected(storage.error());
        auto result = (*storage)->create(input.description);
        if (!result) return cxx::unexpected(result.error());
        return reply.complete(std::move(*result));
    }

    RenderResult<void> InstanceCommands::update(const TransformBatch& input, Reply<UpdateTransforms>& reply) noexcept
    {
        auto storage = device_.resources().borrow<InstanceStorage>(input.storage);
        if (!storage) return cxx::unexpected(storage.error());
        auto result = (*storage)->update(input);
        if (!result) return cxx::unexpected(result.error());
        return reply.complete(std::move(*result));
    }

    RenderResult<void> InstanceCommands::query(const InstanceId& input, Reply<QueryInstance>& reply) noexcept
    {
        auto storage = device_.resources().tryGet<render::InstanceStorage>(input.storage);
        if (!storage) return cxx::unexpected(storage.error());
        auto result = storage->query(input);
        if (!result) return cxx::unexpected(result.error());
        return reply.complete(std::move(*result));
    }

    RenderResult<void> InstanceCommands::release(const InstancePublished& input, Reply<ReleaseInstance>& reply) noexcept
    {
        auto storage = device_.resources().borrow<InstanceStorage>(input.instance.storage);
        if (!storage) return cxx::unexpected(storage.error());
        if (auto result = (*storage)->release(input); !result) return result;
        return reply.complete(Ack{});
    }

    StorageFactory StorageProvider<render::InstanceStorage>::factory() noexcept { return instanceStorageFactory(); }

    StorageCppType StorageProvider<render::InstanceStorage>::implementationType() noexcept
    {
        return storageCppType<Storage>();
    }
}
