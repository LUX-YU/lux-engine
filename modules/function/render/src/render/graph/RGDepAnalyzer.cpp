#include <lux/engine/render/graph/RGDepAnalyzer.hpp>

#include <queue>
#include <algorithm>
#include <limits>
#include <unordered_map>

namespace lux::render
{
    namespace
    {
        struct PassResourceUsage
        {
            bool any_read = false;
            bool any_write = false;
        };

        // Pre-built index entry: which pass uses a resource and how.
        struct ResourcePassEntry
        {
            uint32_t          pass_idx;
            PassResourceUsage usage;
        };

        // Classify a single ERGResourceUsage into read/write flags.
        void accumulateUsage(PassResourceUsage& out, ERGResourceUsage u)
        {
            switch (u)
            {
            case ERGResourceUsage::READ:      out.any_read  = true; break;
            case ERGResourceUsage::WRITE:     out.any_write = true; break;
            case ERGResourceUsage::READ_WRITE:
                out.any_read = true;
                out.any_write = true;
                break;
            }
        }
    } // anonymous namespace

    RGDependencyInfo DependencyAnalyzer::analyze(const RGGraphDescription& graph)
    {
        RGDependencyInfo info{};

        const uint32_t pass_count     = static_cast<uint32_t>(graph.passes.size());
        const uint32_t resource_count = static_cast<uint32_t>(graph.resources.size());

        if (pass_count == 0)
            return info;

        // --------------------------------------------------------
        // 0) Pre-build resource → pass index.  Single pass over all
        //    pass refs: O(P × T) total, replacing the previous
        //    O(R × P × T) collectUsageForResource() linear scans.
        // --------------------------------------------------------
        std::vector<std::vector<ResourcePassEntry>> res_to_passes(resource_count);

        for (uint32_t p = 0; p < pass_count; ++p)
        {
            const auto& pass = graph.passes[p];

            // Temporary per-resource usage within this pass (keyed by resource index).
            // Using a local map avoids duplicate entries when a pass references the
            // same resource in multiple texture/buffer slots.
            std::unordered_map<uint32_t, PassResourceUsage> local_usage;

            for (const auto& tex : pass.textures)
            {
                if (tex.resource.index < resource_count)
                    accumulateUsage(local_usage[tex.resource.index], tex.usage);
            }

            for (const auto& buf : pass.buffers)
            {
                if (buf.resource.index < resource_count)
                    accumulateUsage(local_usage[buf.resource.index], buf.usage);
            }

            for (const auto& [res_idx, usage] : local_usage)
                res_to_passes[res_idx].push_back({p, usage});
        }

        // --------------------------------------------------------
        // 1) Build pass dependency graph based on resource read/write relationships
        //    Adjacency list adj[u] = { all passes v that must execute after u }
        // --------------------------------------------------------
        std::vector<std::vector<uint32_t>> adj(pass_count);
        std::vector<uint32_t> indegree(pass_count, 0);

        // 1a) Compute iteration order that respects explicit after() constraints.
        //     This determines which pass is considered "earlier" for WAW chain
        //     building.  Without after() constraints, passes iterate in their
        //     original declaration order (preserving existing behaviour).

        // Build name → index map (shared by 1a and 1c)
        std::unordered_map<std::string, uint32_t> name_map;
        name_map.reserve(pass_count);
        for (uint32_t i = 0; i < pass_count; ++i)
            name_map[graph.passes[i].name] = i;

        std::vector<uint32_t> iteration_order;
        {

            // Adjacency from after() constraints
            std::vector<std::vector<uint32_t>> after_adj(pass_count);
            std::vector<uint32_t> after_indeg(pass_count, 0);

            for (uint32_t i = 0; i < pass_count; ++i)
            {
                for (const auto& dep_name : graph.passes[i].after_passes)
                {
                    auto it = name_map.find(dep_name);
                    if (it != name_map.end())
                    {
                        after_adj[it->second].push_back(i);  // dep → i
                        ++after_indeg[i];
                    }
                }
            }

            // Kahn's sort. Tie-break ready passes by painter STAGE first, then by
            // original declaration index. stage default = Opaque, so passes that do
            // NOT set a stage keep pure declaration-order tie-break (unchanged
            // behaviour); annotated passes (e.g. Skybox=Sky, Grid=Overlay) get a
            // deterministic painter order independent of feature REGISTRATION order.
            using Key   = std::pair<uint16_t, uint32_t>;  // (stage, original_index)
            using Entry = std::pair<Key, uint32_t>;       // (key, pass_index)
            std::priority_queue<Entry, std::vector<Entry>, std::greater<>> pq;
            auto stage_key = [&](uint32_t i) -> Key {
                return { static_cast<uint16_t>(graph.passes[i].stage), i };
            };
            for (uint32_t i = 0; i < pass_count; ++i)
            {
                if (after_indeg[i] == 0)
                    pq.push({ stage_key(i), i });
            }

            iteration_order.reserve(pass_count);
            while (!pq.empty())
            {
                const uint32_t u = pq.top().second;
                pq.pop();
                iteration_order.push_back(u);
                for (uint32_t v : after_adj[u])
                {
                    if (--after_indeg[v] == 0)
                        pq.push({ stage_key(v), v });
                }
            }

            // Append any remaining (cycle in after-constraints — shouldn't happen)
            if (iteration_order.size() < pass_count)
            {
                std::vector<bool> visited(pass_count, false);
                for (uint32_t idx : iteration_order) visited[idx] = true;
                for (uint32_t i = 0; i < pass_count; ++i)
                    if (!visited[i]) iteration_order.push_back(i);
            }
        }

        // Build pass_idx → position-in-iteration_order lookup for sorting the
        // pre-built resource→pass entries in the correct WAW chain order.
        std::vector<uint32_t> pass_to_order_pos(pass_count);
        for (uint32_t pos = 0; pos < static_cast<uint32_t>(iteration_order.size()); ++pos)
            pass_to_order_pos[iteration_order[pos]] = pos;

        // Sort each resource's pass list by iteration order position so that
        // WAW chains are built in the correct sequence.
        for (auto& entries : res_to_passes)
        {
            std::sort(entries.begin(), entries.end(),
                      [&](const ResourcePassEntry& a, const ResourcePassEntry& b)
                      { return pass_to_order_pos[a.pass_idx] < pass_to_order_pos[b.pass_idx]; });
        }

        // 1b) Build WAW/RAW edges using the pre-built resource→pass index
        for (uint32_t res_idx = 0; res_idx < resource_count; ++res_idx)
        {
            int32_t last_writer = -1;

            for (const auto& entry : res_to_passes[res_idx])
            {
                const uint32_t pass_idx = entry.pass_idx;
                const auto& usage = entry.usage;

                // Write -> Read dependency
                if (usage.any_read && last_writer >= 0 && static_cast<uint32_t>(last_writer) != pass_idx)
                {
                    adj[last_writer].push_back(pass_idx);
                }

                // Write -> Write dependency (must execute sequentially)
                if (usage.any_write)
                {
                    if (last_writer >= 0 && static_cast<uint32_t>(last_writer) != pass_idx)
                    {
                        adj[last_writer].push_back(pass_idx);
                    }
                    last_writer = static_cast<int32_t>(pass_idx);
                }
            }
        }

        // 1c) Add explicit edges from after() constraints
        {
            for (uint32_t i = 0; i < pass_count; ++i)
            {
                for (const auto& dep_name : graph.passes[i].after_passes)
                {
                    auto it = name_map.find(dep_name);
                    if (it != name_map.end())
                        adj[it->second].push_back(i);
                }
            }
        }

        // Deduplicate edges to prevent duplicates
        for (auto& edges : adj)
        {
            std::sort(edges.begin(), edges.end());
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        }

        // Compute in-degrees
        for (uint32_t u = 0; u < pass_count; ++u)
        {
            for (uint32_t v : adj[u])
            {
                indegree[v]++;
            }
        }

        // --------------------------------------------------------
        // 2) Topological sort (Kahn's algorithm) — deterministic tie-breaking (M4)
        //    Tie-breaking key: (phase_mask ascending, name ascending, original index)
        //    This ensures identical graphs always produce the same execution order.
        //    Painter-order (stage) is NOT applied here: stage is consumed by the FIRST
        //    Kahn pass (iteration_order) to build the WAW edges, so stage ordering is
        //    already encoded as hard dependencies by the time we reach this sort.
        // --------------------------------------------------------
        auto cmp = [&](uint32_t a, uint32_t b) {
            const auto& pa = graph.passes[a];
            const auto& pb = graph.passes[b];
            if (pa.phase_mask != pb.phase_mask)
                return pa.phase_mask > pb.phase_mask;
            if (pa.name != pb.name)
                return pa.name > pb.name;
            return a > b;
        };
        std::priority_queue<uint32_t, std::vector<uint32_t>, decltype(cmp)> q(cmp);
        for (uint32_t i = 0; i < pass_count; ++i)
        {
            if (indegree[i] == 0)
                q.push(i);
        }

        info.pass_topological_order.reserve(pass_count);

        while (!q.empty())
        {
            uint32_t u = q.top();
            q.pop();
            info.pass_topological_order.push_back(u);

            for (uint32_t v : adj[u])
            {
                if (--indegree[v] == 0)
                    q.push(v);
            }
        }

        // Cycles are treated as compile-time errors by RenderGraphCompiler.
        // Keep the partial order for diagnostics only.
        if (info.pass_topological_order.size() < pass_count)
        {
            info.has_cycle = true;
        }

        // --------------------------------------------------------
        // 3) Resource lifetime: first_pass / last_pass are both original pass indices.
        //    Uses the pre-built res_to_passes index — O(entries) per resource.
        // --------------------------------------------------------
        info.lifetime.reserve(resource_count);

        for (uint32_t res_idx = 0; res_idx < resource_count; ++res_idx)
        {
            const auto& entries = res_to_passes[res_idx];
            if (entries.empty())
                continue;

            RGTransientLifetime life{};
            life.resource   = RGResourceHandle{ res_idx };
            life.first_pass = std::numeric_limits<uint32_t>::max();
            life.last_pass  = 0;

            for (const auto& entry : entries)
            {
                if (entry.pass_idx < life.first_pass)
                    life.first_pass = entry.pass_idx;
                if (entry.pass_idx > life.last_pass)
                    life.last_pass = entry.pass_idx;
            }

            info.lifetime.push_back(life);
        }

        return info;
    }
}
