// ============================================================================
//  FlowForgeCompilerService.cpp — editor-side FlowGraph precompile/cook cache.
//
//  Pipeline per graph asset:
//    1. content hash = fnv1a64(FlowGraphCodec bytes)  → cache stem "<name>_<hash>"
//    2. miss → compileToObject + linkSharedLibrary into the cache dir, plus a
//       .manifest sidecar carrying what the ABI cannot (per-instance state
//       recipe + event list). Saves precompile in the background; Play remains
//       STARTING until that typed async operation is adopted on the main thread.
//    3. hit  → reuse the cooked native module during export.
//
//  The JIT path is editor tooling only. lld-link missing = loud failure.
// ============================================================================

#include "script/FlowForgeCompilerService.hpp"

#include <lux/engine/platform/FormatCompat.h>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/authoring/assets/FlowGraphAsset.hpp>
#include <lux/engine/authoring/assets/FlowGraphSerDeser.hpp>
#include <lux/engine/function/script/abi/lux_script_abi.h>     // LUX_SCRIPT_ABI_VERSION

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifdef LUX_FLOWFORGE_HAS_MLIR
#include <lux/engine/authoring/flowforge/FlowGraph.hpp>
#include <lux/engine/toolchain/flowforge/mlir/AOT.hpp>
#include <lux/engine/toolchain/flowforge/mlir/IR.hpp>
#include <uuid.h>

#include <unordered_set>
#endif

namespace lux::editor
{
#ifdef LUX_FLOWFORGE_HAS_MLIR

    namespace
    {
        std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept
        {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const std::byte b : bytes)
            {
                h ^= static_cast<std::uint64_t>(b);
                h *= 0x100000001b3ull;
            }
            return h;
        }

        std::string hex64(std::uint64_t v)
        {
            return lux::format("{:016x}", v);
        }

        /// File-name-safe graph name (same alphabet the scratch loader uses).
        std::string sanitizeName(std::string_view name)
        {
            std::string out;
            out.reserve(name.size());
            for (const char c : name)
            {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-')
                    out.push_back(c);
            }
            return out.empty() ? std::string("graph") : out;
        }

        // ── manifest sidecar ────────────────────────────────────────────────
        // Carries what lux_script_abi does NOT expose: the per-instance state
        // recipe + the event list. A pure cache derivative — unreadable or
        // version-mismatched sidecars are treated as a miss and recompiled.
        constexpr std::uint32_t kSidecarMagic   = 0x4D41464Cu;   // 'LFAM'
        constexpr std::uint32_t kSidecarVersion = 1u;

        struct SidecarData
        {
            std::uint32_t          abi_version = 0;
            std::uint32_t          state_size  = 0;
            std::uint64_t          state_hash  = 0;
            std::vector<std::byte> state_defaults;
            std::vector<std::pair<std::string, std::uint32_t>> events;   // name, argc
        };

        bool writeSidecar(const std::filesystem::path& path, const SidecarData& d)
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
            const auto u64 = [&](std::uint64_t v) { out.write(reinterpret_cast<const char*>(&v), 8); };
            u32(kSidecarMagic);
            u32(kSidecarVersion);
            u32(d.abi_version);
            u32(d.state_size);
            u64(d.state_hash);
            u32(static_cast<std::uint32_t>(d.state_defaults.size()));
            if (!d.state_defaults.empty())
                out.write(reinterpret_cast<const char*>(d.state_defaults.data()),
                          static_cast<std::streamsize>(d.state_defaults.size()));
            u32(static_cast<std::uint32_t>(d.events.size()));
            for (const auto& [name, argc] : d.events)
            {
                u32(static_cast<std::uint32_t>(name.size()));
                out.write(name.data(), static_cast<std::streamsize>(name.size()));
                u32(argc);
            }
            return static_cast<bool>(out);
        }

    } // namespace

    struct FlowForgeCompileJob final
    {
        std::filesystem::path      cache_dir;
        std::string               stem;
        std::string               module_name;
        std::string               graph_id;
        std::uint64_t             content_hash{0};
        lux::flowforge::FlowGraph graph;
    };

    struct FlowForgeCompilerService::Impl
    {
        std::filesystem::path      cache_dir;

        // ── compile coordination ────────────────────────────────────────────
        // Main-thread pending/failed sets coalesce each cache stem. The actual
        // compiler receives an immutable job on AsyncRuntime's CPU pool.
        FlowForgeCompileClient compile_client;
        std::unordered_set<std::string> pending;
        std::unordered_set<std::string> failed;

        // No private queue, worker or lock lives here. Admission/coalescing is
        // main-owned; CPU scheduling and terminal delivery belong to
        // AsyncRuntime/EditorAsyncService.
        bool warned_no_client{false};

        Impl(
            std::filesystem::path dir,
            FlowForgeCompileClient client)
            : cache_dir(std::move(dir)), compile_client(std::move(client))
        {
        }

        // ── cache paths ─────────────────────────────────────────────────────
        std::filesystem::path dllPath(const std::string& stem) const
        { return cache_dir / (stem + ".dll"); }
        std::filesystem::path sidecarPath(const std::string& stem) const
        { return cache_dir / (stem + ".manifest"); }

        bool cached(const std::string& stem) const
        {
            std::error_code ec;
            return std::filesystem::exists(dllPath(stem), ec)
                && std::filesystem::exists(sidecarPath(stem), ec);
        }

        /// True = the caller claimed the stem and MUST compile (then call
        /// release). False = the stem is cached (possibly after waiting for
        /// another thread's compile — which may have FAILED, so re-check).
        // Admission and de-duplication are main-thread-owned; worker jobs own
        // immutable snapshots and therefore need no coordination lock here.

        // ── the compile (claim held; any thread) ────────────────────────────
        // @p dir 是调用方在锁下取的目录快照:cache_dir 可被主线程 setCacheDir
        // 重指(openProject 把缓存挪到工程根),worker 不得实时读成员。
        static bool compileAndStore(const std::filesystem::path& dir,
                             const std::string& stem,
                             const std::string& module_name,
                             const lux::flowforge::FlowGraph& graph,
                             std::string* err_out)
        {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);

            lux::flowforge::AotOptions opts;
            opts.module_name = module_name;

            lux::flowforge::AotArtifact artifact;
            std::string err;
            lux::flowforge::IRContext ir_ctx;   // fresh per compile (thread-owned)
            if (!lux::flowforge::compileToObject(ir_ctx, graph, opts, artifact, &err))
            {
                if (err_out) *err_out = "compileToObject: " + err;
                return false;
            }
            if (artifact.state_size > 1024u * 1024u)
            {
                if (err_out) *err_out = "state block exceeds 1 MiB";
                return false;
            }
            if (!lux::flowforge::linkSharedLibrary(artifact, dir / (stem + ".dll"), opts, &err))
            {
                // Missing lld-link fails loudly right here — deliberately NO JIT fallback.
                if (err_out) *err_out = "linkSharedLibrary: " + err;
                return false;
            }

            SidecarData sc;
            sc.abi_version    = LUX_SCRIPT_ABI_VERSION;
            sc.state_size     = static_cast<std::uint32_t>(artifact.state_size);
            sc.state_hash     = artifact.state_hash;
            sc.state_defaults = artifact.state_defaults;
            sc.events.reserve(artifact.events.size());
            for (const auto& ev : artifact.events)
                sc.events.emplace_back(ev.name,
                                       static_cast<std::uint32_t>(ev.arg_count));
            if (!writeSidecar(dir / (stem + ".manifest"), sc))
            {
                if (err_out) *err_out = "manifest sidecar write failed";
                std::filesystem::remove(dir / (stem + ".dll"), ec);
                return false;
            }

            // Counterpart to the startup sweep: retire other GENERATIONS of
            // the same graph name (best effort — a generation loaded by a
            // running play session is never file-locked, the native backend
            // loads BYTES).
            const std::string prefix =
                stem.substr(0, stem.find_last_of('_') + 1);
            for (const auto& entry :
                 std::filesystem::directory_iterator(dir, ec))
            {
                const std::string fname = entry.path().filename().string();
                if (fname.starts_with(prefix) && !fname.starts_with(stem))
                    std::filesystem::remove(entry.path(), ec);
            }
            return true;
        }

    };

    FlowForgeCompileResult compileFlowForgeJob(
        const FlowForgeCompileJob& job) noexcept
    {
        FlowForgeCompileResult result{
            .stem = job.stem,
            .graph_id = job.graph_id};
        std::error_code exists_error;
        const bool cached = std::filesystem::exists(
                job.cache_dir / (job.stem + ".dll"),
                exists_error)
            && std::filesystem::exists(
                job.cache_dir / (job.stem + ".manifest"),
                exists_error);
        if (cached)
        {
            result.ok = true;
            return result;
        }
        result.ok = FlowForgeCompilerService::Impl::compileAndStore(
            job.cache_dir,
            job.stem,
            job.module_name,
            job.graph,
            &result.error);
        return result;
    }

    FlowForgeCompilerService::FlowForgeCompilerService(
        std::filesystem::path cache_dir,
        FlowForgeCompileClient compile_client)
        : impl_(std::make_unique<Impl>(
              std::move(cache_dir),
              std::move(compile_client)))
    {
    }

    FlowForgeCompilerService::~FlowForgeCompilerService() = default;

    void FlowForgeCompilerService::setCacheDir(std::filesystem::path dir)
    {
        // openProject 把 AOT 缓存挪到工程根(此前恒落 cwd/.lux —— 换工程不换
        // 缓存,是被已删的 EditorConfig::project_root 恒空字段藏住的缺口)。
        impl_->cache_dir = std::move(dir);
    }

    namespace
    {
        /// Resolve a FLOW_GRAPH asset and derive its cache identity.
        /// Fails loudly through the returned bool; outputs are the decoded
        /// graph (borrowed from the asset), display name and content hash.
        bool resolveGraph(lux::asset::AssetManager& assets,
                          const std::string&        graph_id,
                          const lux::flowforge::FlowGraph** graph_out,
                          std::string*  module_name_out,
                          std::uint64_t* hash_out)
        {
            const auto id = uuids::uuid::from_string(graph_id);
            if (!id) return false;
            auto loaded = assets.ensureAsset(*id);
            if (!loaded)
            {
                std::fprintf(stderr,
                    "[FlowForgeCompilerService] FLOW_GRAPH %s failed to load\n",
                    graph_id.c_str());
                return false;
            }
            const auto* fga =
                loaded.value()->as<lux::authoring::FlowGraphAsset>();
            if (!fga || !fga->data()) return false;

            std::string err;
            const auto blob =
                lux::authoring::FlowGraphSerDeser::encodeGraph(
                    fga->data()->graph, &err);
            if (blob.empty())
            {
                std::fprintf(stderr,
                    "[FlowForgeCompilerService] graph %s is not serializable: %s\n",
                    graph_id.c_str(), err.c_str());
                return false;
            }

            std::string display = "graph";
            if (const auto* info = assets.queryInfo(*id);
                info && info->display_name[0] != '\0')
                display = info->display_name;

            *graph_out       = &fga->data()->graph;
            *module_name_out = sanitizeName(display);
            *hash_out        = fnv1a64(blob);
            return true;
        }
    } // namespace

    void FlowForgeCompilerService::precompile(lux::asset::AssetManager& assets,
                                            const lux::asset::asset_id_t& id)
    {
        if (impl_->cache_dir.empty() || id.is_nil())
            return;

        const std::string graph_id = uuids::to_string(id);
        const lux::flowforge::FlowGraph* graph = nullptr;
        std::string   module_name;
        std::uint64_t hash = 0;
        if (!resolveGraph(assets, graph_id, &graph, &module_name, &hash))
            return;

        const std::string stem = module_name + "_" + hex64(hash);
        if (impl_->cached(stem) || impl_->pending.contains(stem) ||
            impl_->failed.contains(stem))
            return;

        // 池任务要自己的图克隆(资产那份留在主线程)。
        auto job = std::make_shared<FlowForgeCompileJob>();
        job->cache_dir    = impl_->cache_dir;
        job->stem         = stem;
        job->module_name  = module_name;
        job->graph_id     = graph_id;
        job->content_hash = hash;
        std::string err;
        if (!lux::authoring::FlowGraphSerDeser::cloneGraph(
                *graph, job->graph, &err))
            return;   // resolveGraph already proved serializability; defensive

        if (!impl_->compile_client.valid())
        {
            if (!impl_->warned_no_client)
            {
                impl_->warned_no_client = true;
                std::fprintf(
                    stderr,
                    "[FlowForgeCompilerService] async precompile client is "
                    "unavailable\n");
            }
            return;
        }
        impl_->pending.insert(stem);
        const auto failed_stem = stem;
        const auto failed_graph_id = graph_id;
        if (!impl_->compile_client.submit(
                CompileFlowForgeOperation{std::move(job)},
                [this, failed_stem, failed_graph_id](auto outcome) mutable noexcept
                {
                    if (!outcome)
                    {
                        adoptPrecompileResult(FlowForgeCompileResult{
                            .stem = failed_stem,
                            .graph_id = failed_graph_id,
                            .error = "FlowForge compile operation failed"});
                        return;
                    }
                    adoptPrecompileResult(std::move(*outcome));
                }))
            impl_->pending.erase(stem);
    }

    void FlowForgeCompilerService::adoptPrecompileResult(
        FlowForgeCompileResult result)
    {
        impl_->pending.erase(result.stem);
        if (result.ok)
        {
            impl_->failed.erase(result.stem);
            return;
        }
        impl_->failed.insert(result.stem);
        std::fprintf(
            stderr,
            "[FlowForgeCompilerService] precompile of graph %s FAILED: %s\n",
            result.graph_id.c_str(),
            result.error.c_str());
    }

#else // !LUX_FLOWFORGE_HAS_MLIR — loud no-op stub (compiler disabled)

    struct FlowForgeCompilerService::Impl {};
    FlowForgeCompilerService::FlowForgeCompilerService(
        std::filesystem::path,
        FlowForgeCompileClient)
        : impl_(std::make_unique<Impl>()) {}
    FlowForgeCompilerService::~FlowForgeCompilerService() = default;
    void FlowForgeCompilerService::setCacheDir(std::filesystem::path) {}

    void FlowForgeCompilerService::precompile(lux::asset::AssetManager&,
                                            const lux::asset::asset_id_t&) {}

    void FlowForgeCompilerService::adoptPrecompileResult(
        FlowForgeCompileResult) {}

    FlowForgeCompileResult compileFlowForgeJob(
        const FlowForgeCompileJob&) noexcept
    {
        return FlowForgeCompileResult{
            .error = "FlowForge MLIR compiler is disabled"};
    }

#endif

} // namespace lux::editor
