#pragma once

#include <exec/task.hpp>
#include <lux/engine/render/protocol/CommandMethod.hpp>

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

        exec::task<SetTexture::Response> setTexture(SetTexture::Request&&) noexcept;
        exec::task<SetTextureRegion::Response> updateTextureRegion(SetTextureRegion::Request&&) noexcept;
        void releaseTexture(const ReleaseTexture::Request&, ReleaseTexture::Response&) noexcept;
        void queryTexture(const QueryTexture::Request&, QueryTexture::Response&) noexcept;

        using Bindings = std::tuple<
            CommandMethod<SetTexture, &TextureCommandHandler::setTexture>,
            CommandMethod<SetTextureRegion, &TextureCommandHandler::updateTextureRegion>,
            CommandMethod<ReleaseTexture, &TextureCommandHandler::releaseTexture>,
            CommandMethod<QueryTexture, &TextureCommandHandler::queryTexture>>;

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
