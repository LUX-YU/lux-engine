#include <lux/engine/render/storage/RasterMaterialCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMaterialStorage.hpp>

namespace lux::render::detail
{

    void RasterMaterialCommandHandler::releaseMaterial(
        RasterMaterialStorage& storage,
        const MaterialCommands::Release::Request& input,
        MaterialCommands::Release::Response& response
    ) noexcept
    {
        const auto released = storage.release(input.data);
        response.error = released ? RenderError{} : released.error();
    }
} // namespace lux::render::detail
