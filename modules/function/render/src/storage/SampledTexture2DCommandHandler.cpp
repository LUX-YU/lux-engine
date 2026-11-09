#include <lux/engine/render/storage/SampledTexture2DCommandHandler.hpp>
#include <lux/engine/render/storage/SampledTexture2DStorage.hpp>

namespace lux::render::detail
{

    void SampledTexture2DCommandHandler::releaseTexture(
        SampledTexture2DStorage& storage,
        const ImageCommands::Release::Request& input,
        ImageCommands::Release::Response& response
    ) noexcept
    {
        const auto released = storage.release(input.data);
        response.error = released ? RenderError{} : released.error();
    }
} // namespace lux::render::detail
