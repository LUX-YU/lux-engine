#pragma once
#include <lux/engine/render/storage/SampledTexture2DStorage.hpp>

#include <lux/engine/render/commands/ImageCommands.hpp>

#include <exec/task.hpp>

#include <lux/engine/render/extensions/resource/SampledTexture2D.hpp>
#include <lux/engine/render/extensions/resource/StorageMeta.hpp>

namespace lux::render
{
    class ResourceRegistry;
}

namespace lux::render::detail
{
    class DeviceSession;
    class SampledTexture2DStorage;

    // Texture operations receive a qualified Storage; they do not resolve routing or own the Storage.
    class SampledTexture2DCommandHandler final
    {
    private:
        static constexpr auto create_texture_response_ = [](RenderResult<TextureId> result
                                                         ) noexcept -> ImageCommands::Upload::Response
        {
            if (!result)
            {
                return {result.error()};
            }
            return {{}, *result};
        };

        static constexpr auto create_patched_texture_response_ = [](RenderResult<TextureId> result
                                                                 ) noexcept -> ImageCommands::CloneRegion::Response
        {
            if (!result)
            {
                return {result.error()};
            }
            return {{}, *result};
        };

    public:
        using UploadSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<TextureId>>>(), create_texture_response_));

        using CloneRegionSender = decltype(stdexec::then(
            std::declval<exec::task<RenderResult<TextureId>>>(),
            create_patched_texture_response_
        ));

        static constexpr std::string_view name = "lux.render.storage.SampledTexture2DCommandHandler";

        static UploadSender
        createTexture(SampledTexture2DStorage& storage, ImageCommands::Upload::Request&& input) noexcept
        {
            return render::upload(storage, std::move(input.data)) | stdexec::then(create_texture_response_);
        }

        static CloneRegionSender
        createPatchedTexture(SampledTexture2DStorage& storage, ImageCommands::CloneRegion::Request&& input) noexcept
        {
            return render::upload(storage, std::move(input.data)) | stdexec::then(create_patched_texture_response_);
        }

        static void releaseTexture(
            SampledTexture2DStorage& storage,
            const ImageCommands::Release::Request& input,
            ImageCommands::Release::Response& response
        ) noexcept;
    };

} // namespace lux::render::detail
