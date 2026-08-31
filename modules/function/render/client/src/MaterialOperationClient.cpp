#include <lux/engine/function/render/client/features/material/MaterialOperation.hpp>

#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace lux::render
{
    namespace
    {
        lux::cxx::expected<RenderRequest<MaterialUploadedReply>, ERenderUploadSubmitError> submitGraphMaterial(
            MaterialUploadClient client,
            const GraphMaterialData& data,
            UploadGraphMaterialPayload payload
        )
        {
            const TypeId operation_id = client.ops().id<UploadGraphMaterialOp>();
            if (operation_id == kInvalidTypeId)
            {
                return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
            }

            return client.session().trySubmit<MaterialUploadedReply>(
                [data, payload, operation_id](RenderUploadClient::Builder& builder) mutable {
                    payload.graph_desc = builder.pushOwnedBytesCopy(
                        reinterpret_cast<const std::byte*>(&data),
                        static_cast<std::uint32_t>(sizeof(GraphMaterialData))
                    );
                    builder.pushPreparedResource(operation_id, payload);
                }
            );
        }
    } // namespace

    lux::cxx::expected<RenderRequest<MaterialUploadedReply>, ERenderUploadSubmitError>
    uploadGraphMaterial(MaterialUploadClient client, const GraphMaterialData& data)
    {
        UploadGraphMaterialPayload payload{};
        return submitGraphMaterial(client, data, payload);
    }

    lux::cxx::expected<RenderRequest<MaterialUploadedReply>, ERenderUploadSubmitError> uploadGraphMaterial(
        MaterialUploadClient client,
        const GraphMaterialData& data,
        ShaderHandle gbuffer_shader,
        ShaderHandle forward_shader,
        std::uint32_t alpha_mode,
        bool double_sided
    )
    {
        UploadGraphMaterialPayload payload{};
        payload.graph_gbuffer_shader = gbuffer_shader;
        payload.graph_forward_shader = forward_shader;
        payload.shader_key = (static_cast<std::uint64_t>(gbuffer_shader.index) << 40) ^
                             (static_cast<std::uint64_t>(gbuffer_shader.gen) << 32) ^
                             (static_cast<std::uint64_t>(forward_shader.index) << 8) ^
                             static_cast<std::uint64_t>(forward_shader.gen);
        if (payload.shader_key == 0)
            payload.shader_key = 1;
        payload.alpha_mode = alpha_mode;
        payload.double_sided = double_sided ? 1u : 0u;
        return submitGraphMaterial(client, data, payload);
    }

    lux::cxx::expected<void, ERenderUploadSubmitError>
    modifyGraphMaterial(MaterialUploadClient client, RMaterialHandle handle, const GraphMaterialData& data)
    {
        ModifyGraphMaterialPayload payload{};
        payload.handle = handle;
        return client.modifyGraphMaterial(
            payload,
            std::span<const std::byte>{reinterpret_cast<const std::byte*>(&data), sizeof(GraphMaterialData)},
            alignof(GraphMaterialData)
        );
    }
} // namespace lux::render
