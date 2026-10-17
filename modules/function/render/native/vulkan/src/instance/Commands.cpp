#include <lux/engine/render/native/vulkan/instance/InstanceCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/instance/InstanceStorage.hpp>

namespace lux::render::vulkan
{
    void InstanceCommandHandler::createInstance(
        const CreateInstance::Request& input,
        CreateInstance::Response& response
    ) noexcept
    {
        auto storage = device_.resources().borrow<InstanceStorage>(input.storage);
        if (!storage)
        {
            response = cxx::unexpected(storage.error());
            return;
        }
        auto result = (*storage)->create(input.description);
        if (!result)
        {
            response = cxx::unexpected(result.error());
            return;
        }
        response = std::move(*result);
    }

    void InstanceCommandHandler::updateTransforms(
        const UpdateTransforms::Request& input,
        UpdateTransforms::Response& response
    ) noexcept
    {
        auto storage = device_.resources().borrow<InstanceStorage>(input.storage);
        if (!storage)
        {
            response = cxx::unexpected(storage.error());
            return;
        }
        auto result = (*storage)->update(input);
        if (!result)
        {
            response = cxx::unexpected(result.error());
            return;
        }
        response = std::move(*result);
    }

    void InstanceCommandHandler::queryInstance(
        const QueryInstance::Request& input,
        QueryInstance::Response& response
    ) noexcept
    {
        auto storage = device_.resources().tryGet<render::InstanceStorage>(input.storage);
        if (!storage)
        {
            response = cxx::unexpected(storage.error());
            return;
        }
        auto result = storage->query(input);
        if (!result)
        {
            response = cxx::unexpected(result.error());
            return;
        }
        response = std::move(*result);
    }

    void InstanceCommandHandler::releaseInstance(
        const ReleaseInstance::Request& input,
        ReleaseInstance::Response& response
    ) noexcept
    {
        auto storage = device_.resources().borrow<InstanceStorage>(input.instance.storage);
        if (!storage)
        {
            response = cxx::unexpected(storage.error());
            return;
        }
        if (auto result = (*storage)->release(input); !result)
        {
            response = cxx::unexpected(result.error());
            return;
        }
        response = Ack{};
    }

} // namespace lux::render::vulkan

namespace lux::render
{
    StorageFactory StorageProvider<render::InstanceStorage>::factory() noexcept
    {
        return vulkan::instanceStorageFactory();
    }

    StorageCppType StorageProvider<render::InstanceStorage>::implementationType() noexcept
    {
        return storageCppType<Storage>();
    }
} // namespace lux::render
