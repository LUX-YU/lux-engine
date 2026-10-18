#pragma once

#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>
#include <lux/engine/render/protocol/CommandMethod.hpp>

#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/Material.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    class MaterialStorage;

    class MaterialCommandHandler final
    {
    public:
        static constexpr std::string_view name = "lux.render.vulkan.MaterialCommandHandler";

        explicit MaterialCommandHandler(DeviceSession& device) noexcept : device_(device) {}

        GpuWorkSender<SetMaterial::Response> setMaterial(SetMaterial::Request&&) noexcept;
        void queryMaterial(const QueryMaterial::Request&, QueryMaterial::Response&) noexcept;
        void releaseMaterial(const ReleaseMaterial::Request&, ReleaseMaterial::Response&) noexcept;
        using Bindings = std::tuple<
            CommandMethod<SetMaterial, &MaterialCommandHandler::setMaterial>,
            CommandMethod<QueryMaterial, &MaterialCommandHandler::queryMaterial>,
            CommandMethod<ReleaseMaterial, &MaterialCommandHandler::releaseMaterial>>;

    private:
        DeviceSession& device_;
    };

} // namespace lux::render::vulkan

namespace lux::render
{
    template <class Declaration> struct StorageProvider;

    template <> struct StorageProvider<render::MaterialStorage> final
    {
        using Storage = vulkan::MaterialStorage;
        using Handler = vulkan::MaterialCommandHandler;
        [[nodiscard]] static StorageFactory factory() noexcept;
        [[nodiscard]] static CppType implementationType() noexcept;
    };
} // namespace lux::render
