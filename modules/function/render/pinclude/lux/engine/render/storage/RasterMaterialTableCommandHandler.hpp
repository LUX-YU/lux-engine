#pragma once

#include <lux/engine/render/extensions/storage/ResourceRegistry.hpp>

#include <lux/engine/render/extensions/CommandMethod.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>

#include <lux/engine/render/extensions/storage/RasterMaterialTable.hpp>
#include <lux/engine/render/extensions/storage/StorageFactory.hpp>

namespace lux::render::detail
{
    class DeviceSession;
    class RasterMaterialTableStorage;

    class RasterMaterialTableCommandHandler final
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
        // Factory registration does not construct an object. Activation borrows the one resource directory.
        static RasterMaterialTableCommandHandler* create(ResourceRegistry& resources) noexcept
        {
            return new RasterMaterialTableCommandHandler(resources);
        }

        using CreateMaterialSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<MaterialId>>>(), create_material_response_));

        using UpdateMaterialSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<Ack>>>(), update_material_response_));

        static constexpr std::string_view name = "lux.render.storage.RasterMaterialTableCommandHandler";

        explicit RasterMaterialTableCommandHandler(ResourceRegistry& resources) noexcept : resources_(resources) {}

        CreateMaterialSender createMaterial(CreateMaterial::Request&& input) noexcept
        {
            return uploadMaterial(std::move(input)) | stdexec::then(create_material_response_);
        }

        UpdateMaterialSender updateMaterial(UpdateMaterial::Request&& input) noexcept
        {
            return uploadMaterialParameters(std::move(input)) | stdexec::then(update_material_response_);
        }

        void releaseMaterial(const ReleaseMaterial::Request&, ReleaseMaterial::Response&) noexcept;
        using Bindings = std::tuple<
            CommandMethod<CreateMaterial, &RasterMaterialTableCommandHandler::createMaterial>,
            CommandMethod<UpdateMaterial, &RasterMaterialTableCommandHandler::updateMaterial>,
            CommandMethod<ReleaseMaterial, &RasterMaterialTableCommandHandler::releaseMaterial>>;

    private:
        exec::task<RenderResult<MaterialId>> uploadMaterial(CreateMaterial::Request&&) noexcept;
        exec::task<RenderResult<Ack>> uploadMaterialParameters(UpdateMaterial::Request&&) noexcept;

        ResourceRegistry& resources_;
    };

} // namespace lux::render::detail
