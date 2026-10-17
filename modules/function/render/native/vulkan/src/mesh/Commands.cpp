#include <lux/engine/render/protocol/AsyncCommand.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshCommands.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>

namespace lux::render::vulkan
{
    RenderResult<void> MeshCommands::set(const SetMesh::Request&, Reply<SetMesh>& reply) noexcept
    {
        auto pending = reply.defer();
        if (!pending) return cxx::unexpected(pending.error());
        auto storage = device_.resources().borrow<MeshStorage>(pending->request().mesh.storage);
        if (!storage) return pending->fail(storage.error());
        auto candidate = render::upload(**storage, pending->request());
        if (!candidate) return pending->fail(candidate.error());
        return pending->completeWhen(GpuWorkSender<MeshPublished>{
            std::move(candidate->first), std::move(candidate->second)
        });
    }

    RenderResult<void> MeshCommands::query(const QueryMesh::Request& input, Reply<QueryMesh>& reply) noexcept
    {
        auto storage = device_.resources().tryGet<render::MeshStorage>(input.storage);
        if (!storage) return cxx::unexpected(storage.error());
        auto result = storage->query(input);
        if (!result) return cxx::unexpected(result.error());
        return reply.complete(std::move(*result));
    }

    RenderResult<void> MeshCommands::release(const ReleaseMesh::Request& input, Reply<ReleaseMesh>& reply) noexcept
    {
        auto storage = device_.resources().borrow<MeshStorage>(input.mesh.storage);
        if (!storage) return cxx::unexpected(storage.error());
        if (auto result = (*storage)->release(input.mesh, input.revision); !result) return result;
        return reply.complete(Ack{});
    }

    StorageFactory StorageProvider<render::MeshStorage>::factory() noexcept { return meshStorageFactory(); }

    StorageCppType StorageProvider<render::MeshStorage>::implementationType() noexcept
    {
        return storageCppType<Storage>();
    }
}
