#include <lux/engine/render/graph/RGDebugPrint.hpp>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/gpu/pipeline/EngineSetShapes.hpp> // domainSetSlot
#include <cstdint>
#include <ostream>
#include <limits>

namespace lux::render
{
    const char* passTypeToString(ERGPassType type)
    {
        switch (type)
        {
        case ERGPassType::GRAPHICS:
            return "GRAPHICS";
        case ERGPassType::COMPUTE:
            return "COMPUTE";
        case ERGPassType::TRANSFER:
            return "TRANSFER";
        case ERGPassType::ASYNC_COMPUTE:
            return "ASYNC_COMPUTE";
        case ERGPassType::ASYNC_TRANSFER:
            return "ASYNC_TRANSFER";
        default:
            return "UNKNOWN";
        }
    }

    const char* resourceTypeToString(ERGResourceType t)
    {
        switch (t)
        {
        case ERGResourceType::TEXTURE:
            return "TEXTURE";
        case ERGResourceType::BUFFER:
            return "BUFFER";
        default:
            return "UNKNOWN";
        }
    }

    static const char* queueTypeToString(ERGQueueType q)
    {
        switch (q)
        {
        case ERGQueueType::GRAPHICS:
            return "GRAPHICS";
        case ERGQueueType::COMPUTE:
            return "COMPUTE";
        case ERGQueueType::TRANSFER:
            return "TRANSFER";
        default:
            return "UNKNOWN";
        }
    }

    static const char* electiveKindToString(EElectiveKind k)
    {
        switch (k)
        {
        case EElectiveKind::NONE:
            return "NONE";
        case EElectiveKind::INTRA_GROUP_ADDITIVE:
            return "INTRA_GROUP_ADDITIVE";
        case EElectiveKind::CONDITIONAL_CHAIN:
            return "CONDITIONAL_CHAIN";
        default:
            return "UNKNOWN";
        }
    }

    static const char* geometryTypeToString(EGeometryType t)
    {
        switch (t)
        {
        case EGeometryType::MESH:
            return "MESH";
        case EGeometryType::INSTANCED_MESH:
            return "INSTANCED_MESH";
        case EGeometryType::POINT_CLOUD:
            return "POINT_CLOUD";
        case EGeometryType::LINE_SEGMENTS:
            return "LINE_SEGMENTS";
        case EGeometryType::VOLUMETRIC:
            return "VOLUMETRIC";
        case EGeometryType::PARTICLES:
            return "PARTICLES";
        case EGeometryType::IMPLICIT_SURFACE:
            return "IMPLICIT_SURFACE";
        default:
            return "UNKNOWN";
        }
    }

    static const char* dsSlotToString(EDescriptorSetSlot s)
    {
        switch (s)
        {
        case EDescriptorSetSlot::Scene:
            return "Scene";
        case EDescriptorSetSlot::Instance:
            return "Instance";
        case EDescriptorSetSlot::Texture:
            return "Texture";
        case EDescriptorSetSlot::Light:
            return "Light";
        case EDescriptorSetSlot::Material:
            return "Material";
        case EDescriptorSetSlot::Particle:
            return "Particle";
        case EDescriptorSetSlot::Compute:
            return "Compute";
        default:
            return "UNKNOWN";
        }
    }

    static const char* imageLayoutToString(VkImageLayout layout)
    {
        switch (layout)
        {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            return "UNDEFINED";
        case VK_IMAGE_LAYOUT_GENERAL:
            return "GENERAL";
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return "COLOR_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return "DEPTH_STENCIL_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            return "DEPTH_STENCIL_READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return "SHADER_READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            return "TRANSFER_SRC_OPTIMAL";
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return "TRANSFER_DST_OPTIMAL";
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            return "PRESENT_SRC_KHR";
        case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL:
            return "READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:
            return "ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ:
            return "RENDERING_LOCAL_READ";
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
            return "DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
            return "DEPTH_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
            return "DEPTH_READ_ONLY_OPTIMAL";
        default:
            return "OTHER";
        }
    }

    void printCompiledGraph(const RGCompiledGraph& compiled, std::ostream& os)
    {
        os << "================ RGCompiledGraph Dump ================\n";
        // 0) Basic information
        const auto& graph = compiled.original_graph;
        const uint32_t pass_count = static_cast<uint32_t>(graph.passes.size());
        const uint32_t resource_count = static_cast<uint32_t>(graph.resources.size());

        os << "Valid: " << std::boolalpha << compiled.valid << "\n";
        os << "Pass count    : " << pass_count << "\n";
        os << "Resource count: " << resource_count << "\n";
        os << "(per-view extents — not stored on compiled graph)\n\n";

        if (compiled.mesh_bucket_layout)
        {
            const auto& mesh = *compiled.mesh_bucket_layout;
            os << "[Mesh Bucket Layout] lanes=" << mesh.lanes.size() << "  indirect_bytes=" << mesh.total_indirect_size
               << "  count_bytes=" << mesh.total_count_size << "\n";
            for (const auto& lane : mesh.lanes)
            {
                os << "  pass=" << lane.pass_index << " mdc=" << lane.mdc_index << " bucket=" << lane.bucket_id
                   << " geometry=" << static_cast<unsigned>(lane.geometry_kind) << " pipeline=0x" << std::hex
                   << reinterpret_cast<std::uintptr_t>(lane.pipeline) << std::dec
                   << " indirect_offset=" << lane.indirect_offset << " count_offset=" << lane.count_offset
                   << (lane.bind_pipeline ? " bind" : "") << "\n";
            }
            os << "\n";
        }

        // 0.5) Descriptor layout plan. Without this section there's no way to
        //      see what the allocator actually produced — only that "no error
        //      was raised," and the allocator's worst failure mode is exactly
        //      to silently merge two unrelated sets into one.
        if (compiled.layout_plan)
        {
            const auto& plan = *compiled.layout_plan;
            os << "[Descriptor Layout Plan]  sets=" << plan.sets.size() << "  hash=0x" << std::hex << plan.plan_hash
               << std::dec << "  relaid=" << plan.templates_relaid << "\n";
            // <=4-set feasibility: measures the widest single pipeline
            // (maxBoundDescriptorSets constrains how many sets one pipeline
            // binds, not how many sets exist across the whole graph).
            os << "  最宽管线 set 数:现状 " << plan.max_sets_current << " → 域合并后 " << plan.max_sets_projected
               << "(乐观) / " << plan.max_sets_projected_strict << "(保守:feature 自有 set 不并入)\n";
            for (const auto& o : plan.over_budget)
                os << "  !! 超预算(乐观):" << o << "\n";
            for (const auto& o : plan.over_budget_strict)
                os << "  !! 超预算(保守):" << o << "\n";
            // Per-stage usage: a separate constraint unrelated to set count (merging sets doesn't reduce it).
            if (plan.worst_stage_name)
            {
                const auto& u = plan.worst_stage;
                os << "  最重 stage(" << plan.worst_stage_name << ",非 UAB):"
                   << "ubo=" << u.uniform_buffer << " ssbo=" << u.storage_buffer << " img=" << u.sampled_image
                   << " simg=" << u.storage_image << " 合计=" << u.total << "\n";
            }
            if (plan.worst_stage_merged_name)
            {
                const auto& m = plan.worst_stage_merged;
                os << "  合并后最重 stage(" << plan.worst_stage_merged_name << "):"
                   << "ubo=" << m.uniform_buffer << " ssbo=" << m.storage_buffer << " img=" << m.sampled_image
                   << " simg=" << m.storage_image << " 合计=" << m.total << "  ← 合并的代价\n";
            }
            for (const auto& o : plan.stage_over_min)
                os << "  !! per-stage 超 Vulkan 最低保证:" << o << "\n";

            // Relocation table: the canonical position in the shader -> its
            // final merged position. This is SpirvPatcher's input, and the
            // authoritative answer to "where should the switched-over shader
            // look for its resources" — print it out to check first, then
            // let it actually drive the patch.
            os << "  [Relocations] canonical → merged(SpirvPatcher 的输入)\n";
            for (const auto& s : plan.sets)
            {
                if (s.kind != EPlannedSetKind::EngineShared)
                    continue;
                const uint32_t dst_set = domainSetSlot(s.domain);
                os << "      set" << s.set << " → set" << dst_set << "  binding +" << s.domain_binding_offset
                   << "  (共 " << s.domain_binding_count << " 条)\n";
            }
            for (const auto& s : plan.sets)
            {
                const char* kind = "private";
                switch (s.kind)
                {
                case EPlannedSetKind::EngineShared:
                    kind = "engine ";
                    break;
                case EPlannedSetKind::FeatureExplicit:
                    kind = "feature";
                    break;
                case EPlannedSetKind::PipelinePrivate:
                    kind = "private";
                    break;
                }
                os << "  [" << kind << "] set=" << s.set;
                if (s.owner_key != 0)
                    os << " owner=0x" << std::hex << s.owner_key << std::dec;
                os << "  bindings=" << s.bindings.size() << (s.update_after_bind ? "  UAB" : "");
                if (s.kind == EPlannedSetKind::EngineShared)
                {
                    static constexpr const char* kDomainName[] = {"GLOBAL", "BINDLESS", "FEATURE", "PASS_LOCAL"};
                    const auto d = static_cast<std::size_t>(s.domain);
                    os << "  域=" << (d < 4 ? kDomainName[d] : "?") << " b@[" << s.domain_binding_offset << ",+"
                       << s.domain_binding_count << ")";
                }
                os << "\n";
                for (const auto& b : s.bindings)
                {
                    os << "      b" << b.binding << "  type=" << static_cast<int>(b.type)
                       << "  count=" << (b.count == 0 ? std::string("runtime") : std::to_string(b.count))
                       << "  stages=0x" << std::hex << b.stages << std::dec;
                    if (!b.name.empty())
                        os << "  '" << b.name << "'";
                    os << "\n";
                }
            }

            // Per-pipeline slot table: the checklist for the migration
            // effort. A gate rejection silently falls back to the old path
            // while the gate itself still shows green, so whether a pipeline
            // "switched or not" has to be printed explicitly — [switched] /
            // [not switched] at a glance. The D slot also lists the member
            // sets living inside it.
            if (!plan.pipelines.empty())
            {
                static constexpr const char* kSetName[] =
                    {"Scene", "Instance", "Texture", "Light", "Material", "Particle", "Compute", "VertexPool"};
                constexpr std::size_t kSetNameCount = std::size(kSetName);
                static constexpr const char* kDomainTag[] = {"GLOBAL", "BINDLESS", "FEATURE", "PASS_LOCAL"};

                os << "  [Pipeline Slots] 逐管线槽表"
                      "(E=引擎 set  X=feature 显式  P=私有  H=洞  D=合并域槽;"
                      "恒等=无可动资源,合并与否同形)\n";
                for (const auto& p : plan.pipelines)
                {
                    os << "      " << (p.legacy ? "[旧路]" : (p.merged ? "[已切]" : (p.identity ? "[恒等]" : "[未切]")))
                       << " " << p.name;
                    if (p.legacy)
                    {
                        os << "  (自带 layout,无槽表)\n";
                        continue;
                    }
                    for (const auto& s : p.slots)
                    {
                        os << "  " << s.slot << ":" << s.kind;
                        if (s.kind == 'E' || s.kind == 'H')
                        {
                            // H's sentinel value = a semantic-less placeholder hole in domain mode, unnamed.
                            if (s.logical < kSetNameCount)
                                os << "·" << kSetName[s.logical];
                        }
                        else if (s.kind == 'D')
                        {
                            os << "·" << (s.logical < 4 ? kDomainTag[s.logical] : "?") << "{";
                            for (std::size_t i = 0; i < s.members.size(); ++i)
                                os << (i ? "," : "") << (s.members[i] < kSetNameCount ? kSetName[s.members[i]] : "?");
                            os << "}";
                        }
                    }
                    os << "\n";
                }
            }

            for (const RenderError& w : plan.warnings)
                os << "  !! " << formatRenderError(renderErrorRegistry(), w) << "\n";
            os << "\n";
        }

        // 1) Topological order / Execution order
        os << "[Execution Order]\n  ";
        for (uint32_t idx : compiled.execution_order)
        {
            os << idx << " ";
        }
        os << "\n\n";

        os << "[Dependency Topological Order]\n  ";
        for (uint32_t idx : compiled.dependency_info.pass_topological_order)
        {
            os << idx << " ";
        }
        os << "\n\n";

        // 2) Resource lifetimes
        os << "[Resource Lifetimes]\n";
        for (const auto& life : compiled.dependency_info.lifetime)
        {
            os << "  Resource #" << life.resource.index << "  first_pass=" << life.first_pass
               << "  last_pass=" << life.last_pass << "\n";
        }
        os << "\n";

        // 3) Logical resource information
        os << "[Logical Resources]\n";
        for (uint32_t i = 0; i < resource_count; ++i)
        {
            const auto& res = graph.resources[i];
            os << "  [" << i << "] \"" << res.name << "\"  type=" << resourceTypeToString(res.type);

            // Lifetime
            switch (res.lifetime)
            {
            case ERGResourceLifetime::PERSISTENT:
                os << "  lifetime=PERSISTENT";
                break;
            case ERGResourceLifetime::TRANSIENT:
                os << "  lifetime=TRANSIENT";
                break;
            case ERGResourceLifetime::IMPORTED:
                os << "  lifetime=IMPORTED";
                break;
            case ERGResourceLifetime::FORWARD_REFERENCE:
                os << "  lifetime=FORWARD_REFERENCE";
                break;
            case ERGResourceLifetime::EXTERNAL:
                os << "  lifetime=EXTERNAL";
                break;
            case ERGResourceLifetime::PING_PONG:
                os << "  lifetime=PING_PONG[ring=" << res.ring_size << " phase=" << res.ring_phase
                   << " peer=" << res.pingpong_peer << "]";
                break;
            }

            // Import info for textures
            if (res.import_info)
            {
                os << "  import[initial=" << imageLayoutToString(res.import_info->initial_layout)
                   << " final=" << imageLayoutToString(res.import_info->final_layout) << "]";
            }
            // Import info for buffers
            if (res.import_buffer_info)
            {
                os << "  import_buf[initial_access=0x" << std::hex << res.import_buffer_info->initial_access
                   << " final_access=0x" << res.import_buffer_info->final_access << std::dec << "]";
            }

            os << "\n";
        }
        os << "\n";

        // 4) Valid resource mapping (physical resources are per-view, not on graph)
        os << "[Valid Resources]\n";
        {
            for (std::size_t i = 0; i < compiled.valid_resources.size(); ++i)
            {
                os << "  resource " << i << " -> " << (compiled.valid_resources[i] ? "valid" : "invalid") << "\n";
            }
        }
        os << "\n";

        // 4b) Slot -> physical resource mapping
        os << "[Slot Resource Mapping]\n";
        for (uint32_t si = 0; si < kTargetSlotCount; ++si)
        {
            uint32_t ri = compiled.slot_resource_idx[si];
            os << "  slot " << si << " -> ";
            if (ri == RGCompiledGraph::kInvalidSlotIdx)
                os << "(none)\n";
            else
                os << "physical " << ri << "\n";
        }
        os << "\n";

        // 4c) Relative-size resources
        if (!compiled.relative_size_resources.empty())
        {
            os << "[Relative-Size Resources] (" << compiled.relative_size_resources.size() << ")\n  ";
            for (uint32_t ri : compiled.relative_size_resources)
                os << ri << " ";
            os << "\n\n";
        }

        // 4d) Dynamic external resources
        if (!compiled.dynamic_external_resources.empty())
        {
            os << "[Dynamic External Resources] (" << compiled.dynamic_external_resources.size() << ")\n  ";
            for (uint32_t ri : compiled.dynamic_external_resources)
                os << ri << " ";
            os << "\n\n";
        }

        // 5) RenderPass Layout (printed by group)
        os << "[RenderPass Groups]\n";
        const auto& rp_layout = compiled.render_pass_layout;
        for (std::size_t gi = 0; gi < rp_layout.groups.size(); ++gi)
        {
            const auto& group = rp_layout.groups[gi];
            const auto& key = group.key;

            os << "  [Group " << gi << "] "
               << "samples=" << key.samples << ", color_count=" << key.color_count
               << ", depth_format=" << static_cast<int>(key.depth_stencil_format) << "\n";

            if (key.color_count > 0)
            {
                os << "    Color formats: ";
                for (uint32_t ci = 0; ci < key.color_count; ++ci)
                {
                    os << static_cast<int>(key.color_formats[ci]);
                    if (ci + 1 < key.color_count)
                        os << ", ";
                }
                os << "\n";
            }

            os << "    Passes in group:\n";
            auto load_ch = [](VkAttachmentLoadOp op) {
                return op == VK_ATTACHMENT_LOAD_OP_CLEAR ? 'C' : op == VK_ATTACHMENT_LOAD_OP_LOAD ? 'L' : 'D';
            };
            auto store_ch = [](VkAttachmentStoreOp op) { return op == VK_ATTACHMENT_STORE_OP_STORE ? 'S' : 'D'; };

            for (const auto& p : group.passes)
            {
                os << "      - pass_index=" << p.pass_index << ", subpass_index=" << p.subpass_index;
                // plain 组的逐 pass op 表(serial 路径的真正消费物)。
                if (!group.local_read && !p.pass_color_load_ops.empty())
                {
                    os << "  ops=";
                    for (std::size_t s = 0; s < p.pass_color_load_ops.size(); ++s)
                    {
                        os << load_ch(p.pass_color_load_ops[s])
                           << (s < p.pass_color_store_ops.size() ? store_ch(p.pass_color_store_ops[s]) : '?');
                        if (s + 1 < p.pass_color_load_ops.size())
                            os << ",";
                    }
                    os << " depth=" << load_ch(p.pass_depth_load_op) << store_ch(p.pass_depth_store_op);
                }
                os << "\n";
            }

            if (group.local_read)
            {
                os << "    local_read scope: colors(load/store)=";
                for (std::size_t s = 0; s < group.union_color_res.size(); ++s)
                {
                    os << "#" << group.union_color_res[s] << ":"
                       << (s < group.union_color_load_ops.size() ? load_ch(group.union_color_load_ops[s]) : '?')
                       << (s < group.union_color_store_ops.size() ? store_ch(group.union_color_store_ops[s]) : '?');
                    if (s + 1 < group.union_color_res.size())
                        os << ", ";
                }
                os << "  depth=#" << group.union_depth_res << ":" << load_ch(group.depth_load_op)
                   << store_ch(group.union_depth_store_op) << "\n";
            }
        }
        os << "\n";

        // 6) Additional print: pass -> group/subpass mapping for debugging
        os << "[Pass -> RenderPassGroup Mapping]\n";
        for (std::size_t pass_idx = 0; pass_idx < rp_layout.pass_to_group.size(); ++pass_idx)
        {
            uint32_t g = rp_layout.pass_to_group[pass_idx];
            uint32_t s = (pass_idx < rp_layout.pass_to_subpass.size()) ? rp_layout.pass_to_subpass[pass_idx]
                                                                       : std::numeric_limits<uint32_t>::max();

            os << "  pass " << pass_idx << " -> ";
            if (g == std::numeric_limits<uint32_t>::max())
            {
                os << "(no graphics group)\n";
            }
            else
            {
                os << "group " << g << ", subpass " << s << "\n";
            }
        }
        os << "\n";

        // 7) Compiled results for each pass (printed in execution order)
        os << "[Compiled Passes]\n";
        for (uint32_t order_i = 0; order_i < compiled.execution_order.size(); ++order_i)
        {
            uint32_t pass_idx = compiled.execution_order[order_i];
            const RGCompiledPass& cpass = compiled.compiled_passes[pass_idx];
            const RGPassDescription* p = cpass.pass;

            os << "------------------------------------------------------\n";
            os << "Pass #" << pass_idx << " (order " << order_i << ")\n";
            if (p)
            {
                os << "  Name : " << p->name << "\n";
                os << "  Type : " << passTypeToString(p->type) << "\n";
                os << "  GeomType: " << geometryTypeToString(p->geometry_type) << "\n";
                os << "  Declared textures: " << p->textures.size() << "  buffers: " << p->buffers.size() << "\n";
            }
            else
            {
                os << "  [WARNING] pass pointer is null\n";
            }

            // Queue & elective
            os << "  Queue          : " << queueTypeToString(cpass.queue_type) << "\n";
            os << "  Elective       : " << electiveKindToString(cpass.elective_kind) << "\n";
            os << "  Has Condition  : " << std::boolalpha << (cpass.condition != nullptr);
            if (cpass.pass && cpass.pass->condition_tag != 0)
                os << "  chain_tag=0x" << std::hex << cpass.pass->condition_tag << std::dec;
            os << "\n";

            // Render state
            os << "  RenderPass idx : " << cpass.render.render_pass.index << "\n";
            os << "  Framebuffer idx: " << cpass.render.framebuffer.index << "\n";
            os << "  Pipeline       : 0x" << std::hex << reinterpret_cast<std::uintptr_t>(cpass.render.pipeline)
               << std::dec << "\n";
            os << "  begin_render_pass: " << std::boolalpha << cpass.render.begin_render_pass << "\n";
            os << "  end_render_pass  : " << std::boolalpha << cpass.render.end_render_pass << "\n";
            os << "  bind_pipeline    : " << std::boolalpha << cpass.render.bind_pipeline << "\n";
            os << "  bind_ds_mask     : 0x" << std::hex << cpass.render.bind_ds_mask << std::dec << " (bits:";
            for (uint32_t b = 0; b < kDescriptorSetCount; ++b)
            {
                if (cpass.render.bind_ds_mask & (1u << b))
                    os << " " << b;
            }
            os << ")\n";

            // Geo pipeline table (only non-null entries)
            {
                bool has_any = false;
                for (uint32_t g = 0; g < PassRenderState::kGeometryTypeCount; ++g)
                {
                    if (cpass.render.geo_pipeline_table[g] != VK_NULL_HANDLE)
                    {
                        if (!has_any)
                        {
                            os << "  Geo Pipelines:\n";
                            has_any = true;
                        }
                        os << "    [" << geometryTypeToString(static_cast<EGeometryType>(g)) << "] 0x" << std::hex
                           << reinterpret_cast<std::uintptr_t>(cpass.render.geo_pipeline_table[g]) << std::dec << "\n";
                    }
                }
            }

            // DS bindings (from pass description)
            if (p && !p->ds_bindings.empty())
            {
                os << "  DS Bindings (" << p->ds_bindings.size() << "):\n";
                for (const auto& dsb : p->ds_bindings)
                {
                    const char* source = "Immutable";
                    switch (dsb.source)
                    {
                    case EDSBindingSource::Immutable:
                        source = "Immutable";
                        break;
                    case EDSBindingSource::Scene:
                        source = "Scene";
                        break;
                    case EDSBindingSource::Transient:
                        source = "Transient";
                        break;
                    case EDSBindingSource::Resource:
                        source = "Resource";
                        break;
                    }
                    const char* mode = "Immutable";
                    switch (dsb.mode)
                    {
                    case EDSBindMode::IMMUTABLE:
                        mode = "Immutable";
                        break;
                    case EDSBindMode::PER_FIF:
                        mode = "PerFIF";
                        break;
                    case EDSBindMode::VERSIONED:
                        mode = "Versioned";
                        break;
                    }
                    os << "    slot=" << dsb.slot << " source=" << source << " mode=" << mode << " immutable=0x"
                       << std::hex << reinterpret_cast<std::uintptr_t>(dsb.immutable_set) << " provider=0x"
                       << reinterpret_cast<std::uintptr_t>(dsb.provider.resource) << std::dec
                       << " transient_idx=" << dsb.transient_ds_index << "\n";
                }
            }

            // Resource slot bindings (compiled)
            if (!cpass.resources.pass_resource_bindings.empty())
            {
                os << "  Resource Slot Bindings (" << cpass.resources.pass_resource_bindings.size() << "):\n";
                for (const auto& [slot, vk_idx] : cpass.resources.pass_resource_bindings)
                {
                    os << "    " << dsSlotToString(slot) << " -> vk_set " << vk_idx << "\n";
                }
            }

            // Read/Write resources
            os << "  Read Images  (" << cpass.resources.read_images.size() << "): ";
            for (auto idx : cpass.resources.read_images)
                os << idx << " ";
            os << "\n";

            os << "  Write Images (" << cpass.resources.write_images.size() << "): ";
            for (auto idx : cpass.resources.write_images)
                os << idx << " ";
            os << "\n";

            os << "  Read Buffers (" << cpass.resources.read_buffers.size() << "): ";
            for (auto idx : cpass.resources.read_buffers)
                os << idx << " ";
            os << "\n";

            os << "  Write Buffers(" << cpass.resources.write_buffers.size() << "): ";
            for (auto idx : cpass.resources.write_buffers)
                os << idx << " ";
            os << "\n";

            // Sync info summary (use prebuilt arrays — compile-time RGBarrierInfo2
            // vectors are freed by buildPrebuiltBarriers)
            const auto& sync = cpass.sync;
            os << "  Barriers: img=" << sync.prebuilt_image_barriers.size()
               << " buf=" << sync.prebuilt_buffer_barriers.size() << "\n";

            // Detailed barrier info
            for (std::size_t bi = 0; bi < sync.prebuilt_image_barriers.size(); ++bi)
            {
                const auto& b = sync.prebuilt_image_barriers[bi];
                uint32_t res =
                    (bi < sync.image_patch_resource_idx.size()) ? sync.image_patch_resource_idx[bi] : UINT32_MAX;
                os << "    img_barrier[" << bi << "] res=" << res << " " << imageLayoutToString(b.oldLayout) << " -> "
                   << imageLayoutToString(b.newLayout) << "\n";
            }
            for (std::size_t bi = 0; bi < sync.prebuilt_buffer_barriers.size(); ++bi)
            {
                uint32_t res =
                    (bi < sync.buffer_patch_resource_idx.size()) ? sync.buffer_patch_resource_idx[bi] : UINT32_MAX;
                os << "    buf_barrier[" << bi << "] res=" << res << "\n";
            }
            // (Split release/acquire barrier dump removed with the split-barrier
            //  machinery — see RenderGraphCompiler.)

            // Cross-queue dependencies
            if (!cpass.wait_dependencies.empty())
            {
                os << "  Wait Dependencies (" << cpass.wait_dependencies.size() << "):\n";
                for (const auto& dep : cpass.wait_dependencies)
                    os << "    src_pass=" << dep.src_pass_index << " val=" << dep.signal_value << "\n";
            }
            if (!cpass.signal_dependencies.empty())
            {
                os << "  Signal Dependencies (" << cpass.signal_dependencies.size() << "):\n";
                for (const auto& dep : cpass.signal_dependencies)
                    os << "    src_pass=" << dep.src_pass_index << " val=" << dep.signal_value << "\n";
            }
        }
        os << "\n";

        // 9) Multi-queue info
        if (compiled.multi_queue_info.has_async_work)
        {
            os << "[Multi-Queue Info]\n";
            os << "  Graphics order (" << compiled.multi_queue_info.graphics_order.size() << "): ";
            for (uint32_t idx : compiled.multi_queue_info.graphics_order)
                os << idx << " ";
            os << "\n";
            os << "  Compute  order (" << compiled.multi_queue_info.compute_order.size() << "): ";
            for (uint32_t idx : compiled.multi_queue_info.compute_order)
                os << idx << " ";
            os << "\n";
            os << "  Transfer order (" << compiled.multi_queue_info.transfer_order.size() << "): ";
            for (uint32_t idx : compiled.multi_queue_info.transfer_order)
                os << idx << " ";
            os << "\n\n";
        }

        // 10) Final barriers (use prebuilt arrays — compile-time vectors are freed)
        if (!compiled.prebuilt_final_barriers.empty())
        {
            os << "[Final Barriers] (" << compiled.prebuilt_final_barriers.size() << ")\n";
            for (std::size_t fi = 0; fi < compiled.prebuilt_final_barriers.size(); ++fi)
            {
                const auto& b = compiled.prebuilt_final_barriers[fi];
                uint32_t res = (fi < compiled.final_patch_resource_idx.size()) ? compiled.final_patch_resource_idx[fi]
                                                                               : UINT32_MAX;
                os << "  [" << fi << "] res=" << res << " " << imageLayoutToString(b.oldLayout) << " -> "
                   << imageLayoutToString(b.newLayout) << "\n";
            }
            os << "\n";
        }

        os << "================ End of RGCompiledGraph Dump ================\n";
    }
} // namespace lux::render
