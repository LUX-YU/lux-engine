#pragma once

#include <exec/task.hpp>
#include <lux/engine/render/commands/CommandMethod.hpp>

#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/Texture.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    class TextureStorage;

    // One device-borrowing object for all Texture Commands. Storage remains separately scope-owned.
    class TextureCommandHandler final
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

        static constexpr std::string_view name = "lux.render.vulkan.TextureCommandHandler";

        explicit TextureCommandHandler(DeviceSession& device) noexcept : device_(device) {}

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
            CommandMethod<CreateTexture, &TextureCommandHandler::createTexture>,
            CommandMethod<CreatePatchedTexture, &TextureCommandHandler::createPatchedTexture>,
            CommandMethod<ReleaseTexture, &TextureCommandHandler::releaseTexture>>;

    private:
        exec::task<RenderResult<TextureId>> createTextureSource(CreateTexture::Request&&) noexcept;
        exec::task<RenderResult<TextureId>> createPatchedTextureSource(CreatePatchedTexture::Request&&) noexcept;

        DeviceSession& device_;
    };

} // namespace lux::render::vulkan

namespace lux::render
{
    template <class Declaration> struct StorageProvider;

    template <> struct StorageProvider<render::TextureStorage> final
    {
        using Storage = vulkan::TextureStorage;
        using Handler = vulkan::TextureCommandHandler;

        [[nodiscard]] static std::unique_ptr<Handler> createHandler(vulkan::DeviceSession& device) noexcept
        {
            return std::make_unique<Handler>(device);
        }

        [[nodiscard]] static StorageFactory factory() noexcept;
        [[nodiscard]] static CppType implementationType() noexcept;
    };
} // namespace lux::render
