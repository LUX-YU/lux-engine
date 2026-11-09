#pragma once
#include <lux/engine/render/storage/RasterMaterialStorage.hpp>

#include <lux/engine/render/commands/MaterialCommands.hpp>

#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>

#include <lux/engine/render/extensions/storage/RasterMaterial.hpp>
#include <lux/engine/render/extensions/storage/StorageFactory.hpp>

namespace lux::render
{
    class ResourceRegistry;
}

namespace lux::render::detail
{
    class DeviceSession;
    class RasterMaterialStorage;

    class RasterMaterialCommandHandler final
    {
    private:
        static constexpr auto create_material_response_ = [](RenderResult<MaterialId> result
                                                          ) noexcept -> MaterialCommands::Create::Response
        {
            if (!result)
            {
                return {result.error()};
            }
            return {{}, *result};
        };

        static constexpr auto update_material_response_ = [](RenderResult<Ack> result
                                                          ) noexcept -> MaterialCommands::Update::Response
        { return {result ? RenderError{} : result.error()}; };

    public:
        // Stateless typed methods; the Server supplies the selected Storage at dispatch.
        static RasterMaterialCommandHandler* create(ResourceRegistry&) noexcept
        {
            return new RasterMaterialCommandHandler;
        }

        using CreateMaterialSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<MaterialId>>>(), create_material_response_));

        using UpdateMaterialSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<Ack>>>(), update_material_response_));

        static constexpr std::string_view name = "lux.render.storage.RasterMaterialCommandHandler";

        CreateMaterialSender
        createMaterial(RasterMaterialStorage& storage, MaterialCommands::Create::Request&& input) noexcept
        {
            return render::upload(storage, std::move(input.data)) | stdexec::then(create_material_response_);
        }

        UpdateMaterialSender
        updateMaterial(RasterMaterialStorage& storage, MaterialCommands::Update::Request&& input) noexcept
        {
            return render::upload(storage, std::move(input.data)) | stdexec::then(update_material_response_);
        }

        void releaseMaterial(
            RasterMaterialStorage& storage,
            const MaterialCommands::Release::Request& input,
            MaterialCommands::Release::Response& response
        ) noexcept;
    };

} // namespace lux::render::detail
