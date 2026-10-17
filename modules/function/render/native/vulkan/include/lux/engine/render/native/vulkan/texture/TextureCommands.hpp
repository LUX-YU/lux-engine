#pragma once

#include <lux/engine/render/protocol/CommandHandler.hpp>

#include <lux/engine/render/resource/Texture.hpp>
#include <lux/engine/render/resource/Factory.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    class TextureStorage;

    // One device-borrowing object for all Texture Commands. Storage remains separately scope-owned.
    class TextureCommands final
    {
    public:
        static constexpr std::string_view name = "lux.render.vulkan.TextureCommands";
        explicit TextureCommands(DeviceSession& device) noexcept : device_(device) {}

        RenderResult<void> set(const SetTexture::Request&, Reply<SetTexture>&) noexcept;
        RenderResult<void> region(const SetTextureRegion::Request&, Reply<SetTextureRegion>&) noexcept;
        RenderResult<void> release(const ReleaseTexture::Request&, Reply<ReleaseTexture>&) noexcept;
        RenderResult<void> query(const QueryTexture::Request&, Reply<QueryTexture>&) noexcept;

        using Bindings = std::tuple<CommandMethod<SetTexture, &TextureCommands::set>,
            CommandMethod<SetTextureRegion, &TextureCommands::region>,
            CommandMethod<ReleaseTexture, &TextureCommands::release>,
            CommandMethod<QueryTexture, &TextureCommands::query>>;

    private:
        DeviceSession& device_;
    };

}

namespace lux::render
{
    template <class Declaration> struct StorageProvider;

    template <> struct StorageProvider<render::TextureStorage> final
    {
        using Storage = vulkan::TextureStorage;
        using Handler = vulkan::TextureCommands;
        [[nodiscard]] static StorageFactory factory() noexcept;
        [[nodiscard]] static StorageCppType implementationType() noexcept;
    };
}
