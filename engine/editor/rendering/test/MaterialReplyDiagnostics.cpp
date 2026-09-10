#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>

namespace lux::render
{
    void handleDestroyMaterial(GeneralRenderServer::Dispatcher::Ctx &, const DestroyMaterialPayload &);
    void handleUploadGraphMaterial(GeneralRenderServer::Dispatcher::Ctx &, const UploadGraphMaterialPayload &);
    void handleModifyGraphMaterial(GeneralRenderServer::Dispatcher::Ctx &, const ModifyGraphMaterialPayload &);
}

// Diagnostic-only calls into the existing private handlers. Compiled in their owning DLL so
// the normal SDK needs no new handler exports and the test never copies their implementation.
extern "C" __declspec(dllexport) void lux_er1_handle_destroy_material(
    lux::render::GeneralRenderServer::Dispatcher::Ctx &context, const lux::render::DestroyMaterialPayload &payload)
{
    lux::render::handleDestroyMaterial(context, payload);
}
extern "C" __declspec(dllexport) void lux_er1_handle_upload_material(
    lux::render::GeneralRenderServer::Dispatcher::Ctx &context, const lux::render::UploadGraphMaterialPayload &payload)
{
    lux::render::handleUploadGraphMaterial(context, payload);
}
extern "C" __declspec(dllexport) void lux_er1_handle_modify_material(
    lux::render::GeneralRenderServer::Dispatcher::Ctx &context, const lux::render::ModifyGraphMaterialPayload &payload)
{
    lux::render::handleModifyGraphMaterial(context, payload);
}
