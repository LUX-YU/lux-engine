#pragma once

#include <lux/engine/render/resource/ResourceRegistry.hpp>

#include <exec/task.hpp>
#include <lux/engine/render/commands/CommandMethod.hpp>

#include <lux/engine/render/resource/StorageFactory.hpp>
#include <lux/engine/render/storage/SampledTexture2D.hpp>

namespace lux::render::detail
{
    class DeviceSession;
    class SampledTexture2DStorage;

    // One resource-directory borrower for all Texture Commands. Storage remains separately scope-owned.
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
        // Factory registration does not construct an object. Activation borrows the one resource directory.
        static SampledTexture2DCommandHandler* create(ResourceRegistry& resources) noexcept
        {
            return new SampledTexture2DCommandHandler(resources);
        }

        using CreateTextureSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<TextureId>>>(), create_texture_response_));

        using CreatePatchedTextureSender = decltype(stdexec::then(
            std::declval<exec::task<RenderResult<TextureId>>>(),
            create_patched_texture_response_
        ));

        static constexpr std::string_view name = "lux.render.storage.SampledTexture2DCommandHandler";

        explicit SampledTexture2DCommandHandler(ResourceRegistry& resources) noexcept : resources_(resources) {}

        CreateTextureSender createTexture(CreateTexture::Request&& input) noexcept
        {
            return uploadTexture(std::move(input)) | stdexec::then(create_texture_response_);
        }

        CreatePatchedTextureSender createPatchedTexture(CreatePatchedTexture::Request&& input) noexcept
        {
            return uploadPatchedTexture(std::move(input)) | stdexec::then(create_patched_texture_response_);
        }

        void releaseTexture(const ReleaseTexture::Request&, ReleaseTexture::Response&) noexcept;
        using Bindings = std::tuple<
            CommandMethod<CreateTexture, &SampledTexture2DCommandHandler::createTexture>,
            CommandMethod<CreatePatchedTexture, &SampledTexture2DCommandHandler::createPatchedTexture>,
            CommandMethod<ReleaseTexture, &SampledTexture2DCommandHandler::releaseTexture>>;

    private:
        exec::task<RenderResult<TextureId>> uploadTexture(CreateTexture::Request&&) noexcept;
        exec::task<RenderResult<TextureId>> uploadPatchedTexture(CreatePatchedTexture::Request&&) noexcept;

        ResourceRegistry& resources_;
    };

} // namespace lux::render::detail
