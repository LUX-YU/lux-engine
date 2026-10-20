#pragma once

#include <exec/task.hpp>
#include <lux/engine/render/commands/CommandMethod.hpp>

#include <lux/engine/render/resource/StorageFactory.hpp>
#include <lux/engine/render/storage/SampledTexture2D.hpp>

namespace lux::render::detail
{
    class DeviceSession;
    class SampledTexture2DStorage;

    // One device-borrowing object for all Texture Commands. Storage remains separately scope-owned.
    class SampledTexture2DCommandHandler final
    {
    private:
        static constexpr auto create_texture_response_ = [](RenderResult<TextureId> result
                                                         ) noexcept -> CreateTexture::Response
        {
            if (!result)
            {
                return {result.error()};
            }
            return {{}, *result};
        };

        static constexpr auto create_patched_texture_response_ = [](RenderResult<TextureId> result
                                                                 ) noexcept -> CreatePatchedTexture::Response
        {
            if (!result)
            {
                return {result.error()};
            }
            return {{}, *result};
        };

    public:
        using CreateTextureSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<TextureId>>>(), create_texture_response_));

        using CreatePatchedTextureSender = decltype(stdexec::then(
            std::declval<exec::task<RenderResult<TextureId>>>(),
            create_patched_texture_response_
        ));

        static constexpr std::string_view name = "lux.render.storage.SampledTexture2DCommandHandler";

        explicit SampledTexture2DCommandHandler(DeviceSession& device) noexcept : device_(device) {}

        CreateTextureSender createTexture(CreateTexture::Request&& input) noexcept
        {
            return createTextureSource(std::move(input)) | stdexec::then(create_texture_response_);
        }

        CreatePatchedTextureSender createPatchedTexture(CreatePatchedTexture::Request&& input) noexcept
        {
            return createPatchedTextureSource(std::move(input)) | stdexec::then(create_patched_texture_response_);
        }

        void releaseTexture(const ReleaseTexture::Request&, ReleaseTexture::Response&) noexcept;
        using Bindings = std::tuple<
            CommandMethod<CreateTexture, &SampledTexture2DCommandHandler::createTexture>,
            CommandMethod<CreatePatchedTexture, &SampledTexture2DCommandHandler::createPatchedTexture>,
            CommandMethod<ReleaseTexture, &SampledTexture2DCommandHandler::releaseTexture>>;

    private:
        exec::task<RenderResult<TextureId>> createTextureSource(CreateTexture::Request&&) noexcept;
        exec::task<RenderResult<TextureId>> createPatchedTextureSource(CreatePatchedTexture::Request&&) noexcept;

        DeviceSession& device_;
    };

} // namespace lux::render::detail
