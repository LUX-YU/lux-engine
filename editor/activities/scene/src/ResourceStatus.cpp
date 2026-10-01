#include <lux/engine/editor/scene/ResourceStatus.hpp>
namespace lux::editor::scene
{
    namespace
    {
        auto missing()
        {
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        }
        auto unavailable(const lux::scene::SceneRuntimeFailure& failure)
        {
            const auto* error = std::get_if<lux::scene::ESceneRuntimeError>(&failure.cause);
            return lux::cxx::unexpected(render::RendererFailure{
                error && *error == lux::scene::ESceneRuntimeError::BUSY ? render::ERendererError::BUSY :
                    render::ERendererError::INVALID_ARGUMENT});
        }
    }
    render::RenderResult<ResourceStatusSnapshot> captureResourceStatus(
        const lux::scene::SceneRuntime& runtime,
        lux::scene::SceneInstanceId instance,
        lux::system::SystemInstanceId system
    )
    {
        auto borrowed = runtime.borrowInstance(instance);
        if (!borrowed)
            return unavailable(borrowed.error());
        const auto* assets = lux::scene::RenderAssets::find(borrowed->get(), system);
        if (!assets)
            return missing();
        auto rows = assets->statuses();
        return ResourceStatusSnapshot{instance, system, assets->revision(), {rows.begin(), rows.end()}};
    }
    render::RenderResult<void> retryResource(lux::scene::SceneRuntime& runtime, ResourceRetryRequest request)
    {
        auto borrowed = runtime.borrowInstance(request.instance);
        if (!borrowed)
            return unavailable(borrowed.error());
        auto* assets = lux::scene::RenderAssets::find(borrowed->get(), request.system);
        if (!assets)
            return missing();
        return assets->retry(request.key);
    }
}
