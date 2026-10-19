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
    public:
        static constexpr std::string_view name = "lux.render.vulkan.TextureCommandHandler";

        explicit TextureCommandHandler(DeviceSession& device) noexcept : device_(device) {}

        exec::task<CreateTexture::Response> createTexture(CreateTexture::Request&&) noexcept;
        exec::task<CreatePatchedTexture::Response> createPatchedTexture(CreatePatchedTexture::Request&&) noexcept;
        void releaseTexture(const ReleaseTexture::Request&, ReleaseTexture::Response&) noexcept;
        using Bindings = std::tuple<
            CommandMethod<CreateTexture, &TextureCommandHandler::createTexture>,
            CommandMethod<CreatePatchedTexture, &TextureCommandHandler::createPatchedTexture>,
            CommandMethod<ReleaseTexture, &TextureCommandHandler::releaseTexture>>;

    private:
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
