#pragma once

#include <lux/engine/render/commands/CommandMethod.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>

#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/Material.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    class MaterialStorage;

    class MaterialCommandHandler final
    {
    private:
        static constexpr auto create_material_response_ = [](RenderResult<MaterialId> result
                                                          ) noexcept -> CreateMaterial::Response
        {
            if (!result)
            {
                return {result.error()};
            }
            return {{}, *result};
        };

        static constexpr auto update_material_response_ = [](RenderResult<Ack> result
                                                          ) noexcept -> UpdateMaterial::Response
        { return {result ? RenderError{} : result.error()}; };

    public:
        using CreateMaterialSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<MaterialId>>>(), create_material_response_));

        using UpdateMaterialSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<Ack>>>(), update_material_response_));

        static constexpr std::string_view name = "lux.render.vulkan.MaterialCommandHandler";

        explicit MaterialCommandHandler(DeviceSession& device) noexcept : device_(device) {}

        CreateMaterialSender createMaterial(CreateMaterial::Request&& input) noexcept
        {
            return createMaterialSource(std::move(input)) | stdexec::then(create_material_response_);
        }

        UpdateMaterialSender updateMaterial(UpdateMaterial::Request&& input) noexcept
        {
            return updateMaterialSource(std::move(input)) | stdexec::then(update_material_response_);
        }

        void releaseMaterial(const ReleaseMaterial::Request&, ReleaseMaterial::Response&) noexcept;
        using Bindings = std::tuple<
            CommandMethod<CreateMaterial, &MaterialCommandHandler::createMaterial>,
            CommandMethod<UpdateMaterial, &MaterialCommandHandler::updateMaterial>,
            CommandMethod<ReleaseMaterial, &MaterialCommandHandler::releaseMaterial>>;

    private:
        exec::task<RenderResult<MaterialId>> createMaterialSource(CreateMaterial::Request&&) noexcept;
        exec::task<RenderResult<Ack>> updateMaterialSource(UpdateMaterial::Request&&) noexcept;

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

        [[nodiscard]] static std::unique_ptr<Handler> createHandler(vulkan::DeviceSession& device) noexcept
        {
            return std::make_unique<Handler>(device);
        }

        [[nodiscard]] static StorageFactory factory() noexcept;
        [[nodiscard]] static CppType implementationType() noexcept;
    };
} // namespace lux::render
