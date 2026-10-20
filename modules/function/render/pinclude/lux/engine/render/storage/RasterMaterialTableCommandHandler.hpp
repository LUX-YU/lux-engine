#pragma once

#include <lux/engine/render/commands/CommandMethod.hpp>
#include <lux/engine/render/upload/UploadContext.hpp>

#include <lux/engine/render/resource/StorageFactory.hpp>
#include <lux/engine/render/storage/RasterMaterialTable.hpp>

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
        using CreateMaterialSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<MaterialId>>>(), create_material_response_));

        using UpdateMaterialSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<Ack>>>(), update_material_response_));

        static constexpr std::string_view name = "lux.render.storage.RasterMaterialTableCommandHandler";

        explicit RasterMaterialTableCommandHandler(DeviceSession& device) noexcept : device_(device) {}

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
            CommandMethod<CreateMaterial, &RasterMaterialTableCommandHandler::createMaterial>,
            CommandMethod<UpdateMaterial, &RasterMaterialTableCommandHandler::updateMaterial>,
            CommandMethod<ReleaseMaterial, &RasterMaterialTableCommandHandler::releaseMaterial>>;

    private:
        exec::task<RenderResult<MaterialId>> createMaterialSource(CreateMaterial::Request&&) noexcept;
        exec::task<RenderResult<Ack>> updateMaterialSource(UpdateMaterial::Request&&) noexcept;

        DeviceSession& device_;
    };

} // namespace lux::render::detail
