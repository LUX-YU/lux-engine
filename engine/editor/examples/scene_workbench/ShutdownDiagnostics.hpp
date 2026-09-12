#pragma once
#include <lux/engine/editor/application/Application.hpp>
#include <lux/engine/editor/sessions/scene/SceneResourceStatus.hpp>
#include <cstdio>

namespace lux::editor::examples
{
    inline void reportShutdown(const rendering::RendererCloseStatus &status)
    {
        std::fprintf(stderr, "shutdown renderer=%u close=%u worker_stopped=%u leases=%zu views=%zu frames=%zu "
                     "uploads=%u program=%u deferred_scene=%zu deferred_view=%zu deferred_target=%zu\n",
                     unsigned(status.state), unsigned(status.close_requested), unsigned(status.worker_stopped),
                     status.statistics.runtime_leases, status.view_count, status.statistics.accepted_frames,
                     unsigned(status.uploads_pending), unsigned(status.program_pending), status.scene_releases,
                     status.view_releases, status.target_releases);
        for (std::size_t i = 0; i < status.view_count; ++i)
        {
            const auto &view = status.views[i];
            std::fprintf(stderr, "shutdown view=%llu/%llu/%llu state=%u version_owners=%ld submitted=%llu "
                         "gpu_completed=%llu requests=%llu,%llu,%llu,%llu,%llu target_owned=%u view_owned=%u\n",
                         view.status.view.renderer, view.status.view.slot, view.status.view.generation,
                         unsigned(view.status.state), view.image_version_owners, view.last_submission,
                         view.gpu_completed, view.create_view, view.create_target, view.resize, view.release_view,
                         view.release_target, unsigned(view.target_owned), unsigned(view.view_owned));
        }
    }
    inline void reportShutdown(const sessions::SceneCloseSnapshot &status)
    {
        std::fprintf(stderr, "shutdown session=%llu state=%u views=%zu scene=%u scope_complete=%u "
                     "retirement_queued=%u resources=%zu\n", status.session.value, unsigned(status.state),
                     status.views, unsigned(status.scene_present), unsigned(status.task_scope_complete),
                     unsigned(status.retirement_submission_pending), status.resources.size());
        for (const auto &row : status.resources)
        {
            const auto &resource = row.resource;
            std::fprintf(stderr, "shutdown resource session=%llu entity=%u sequence=%llu state=%u "
                         "reads=%u,%u uploads=%u,%u,%u,%u handles=%zu retirement=%u backend=%u "
                         "render_error=%u:%u args=%u,%u,%u mesh=", resource.key.target.session.value,
                         unsigned(entt::to_integral(resource.key.target.entity)), resource.key.sequence,
                         unsigned(resource.state), unsigned(row.mesh_read_pending), unsigned(row.material_read_pending),
                         unsigned(row.mesh_upload_pending), unsigned(row.material_upload_pending),
                         unsigned(row.forward_upload_pending), unsigned(row.gbuffer_upload_pending), row.live_handles,
                         unsigned(row.retirement_pending), resource.backend_status, resource.render_failure.type.index,
                         resource.render_failure.type.gen, resource.render_failure.args[0],
                         resource.render_failure.args[1], resource.render_failure.args[2]);
            for (const auto byte : resource.key.mesh.bytes())
                std::fprintf(stderr, "%02x", std::to_integer<unsigned>(byte));
            std::fputs(" material=", stderr);
            for (const auto byte : resource.key.material.bytes())
                std::fprintf(stderr, "%02x", std::to_integer<unsigned>(byte));
            if (resource.asset_failure)
                std::fprintf(stderr, " asset_storage=%u", unsigned(resource.asset_failure->storage_error));
            std::fputc('\n', stderr);
        }
    }
    inline void reportShutdown(const application::ApplicationShutdownStatus &status)
    {
        std::fprintf(stderr, "shutdown application=%u requested=%u workspace=%u unattached_view=%u\n",
                     unsigned(status.state), unsigned(status.close_requested), unsigned(status.workspace_present),
                     unsigned(status.unattached_view_present));
        if (status.renderer)
            reportShutdown(*status.renderer);
        if (status.session)
            reportShutdown(*status.session);
    }
} // namespace lux::editor::examples
