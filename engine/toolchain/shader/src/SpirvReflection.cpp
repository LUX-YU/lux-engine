#include <lux/engine/toolchain/shader/SpirvReflection.hpp>

#include <spirv_cross.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace lux::toolchain
{
    using namespace lux::rdesc;
    namespace sc = SPIRV_CROSS_NAMESPACE;

    // Helper: map to the vertex base type
    static EVertexScalarBase toVertexBase(const sc::SPIRType& t)
    {
        using BT = sc::SPIRType::BaseType;
        switch (t.basetype)
        {
        case BT::Boolean:
            return EVertexScalarBase::BOOL;
        case BT::Int:
            return EVertexScalarBase::INT;
        case BT::UInt:
            return EVertexScalarBase::UINT;
        case BT::Half:
        case BT::Float:
            return EVertexScalarBase::FLOAT;
        case BT::Double:
            return EVertexScalarBase::DOUBLE;
        default:
            return EVertexScalarBase::UNKNOWN;
        }
    }

    static uint32_t calcArraySize(const sc::Compiler& comp, const sc::SPIRType& t)
    {
        if (t.array.empty())
        {
            return 1;
        }
        uint64_t total = 1;
        for (size_t i = 0; i < t.array.size(); ++i)
        {
            if (t.array_size_literal[i])
            {
                total *= t.array[i];
            }
            else
            {
                return 1;
            }
        }
        return static_cast<uint32_t>(total);
    }

    static bool isWritableStorage(const sc::Compiler& comp, uint32_t var_id)
    {
        auto bits = comp.get_decoration_bitset(var_id);
        return !bits.get(spv::DecorationNonWritable);
    }

    static void pushBinding(ShaderInfo& out, const EDescriptorBindingInfo& b)
    {
        size_t idx = out.findSet(b.set);
        if (idx == ShaderInfo::npos)
        {
            out.sets.push_back(DescriptorSetLayoutInfo{b.set, {}});
            idx = out.sets.size() - 1;
        }
        out.sets[idx].bindings.push_back(b);
    }

    static std::vector<EntryPointInfo> detectEntry(sc::Compiler& comp)
    {
        std::vector<EntryPointInfo> out;
        auto entries = comp.get_entry_points_and_stages();
        if (entries.empty())
        {
            return {};
        }

        for (auto& entry : entries)
        {
            EntryPointInfo epi{};
            epi.name = entry.name;
            switch (entry.execution_model)
            {
            case spv::ExecutionModelVertex:
                epi.stage = EShaderType::VERTEX;
                break;
            case spv::ExecutionModelFragment:
                epi.stage = EShaderType::FRAGMENT;
                break;
            case spv::ExecutionModelGLCompute:
                epi.stage = EShaderType::COMPUTE;
                break;
            case spv::ExecutionModelGeometry:
                epi.stage = EShaderType::GEOMETRY;
                break;
            case spv::ExecutionModelTessellationControl:
                epi.stage = EShaderType::TESSELLATION_CTRL;
                break;
            case spv::ExecutionModelTessellationEvaluation:
                epi.stage = EShaderType::TESSELLATION_EVAL;
                break;
            // case spv::ExecutionModelRayGenerationKHR:       return EShaderType::RAYGEN;
            // case spv::ExecutionModelAnyHitKHR:              return EShaderType::ANY_HIT;
            // case spv::ExecutionModelClosestHitKHR:          return EShaderType::CLOSEST_HIT;
            // case spv::ExecutionModelMissKHR:                return EShaderType::MISS;
            // case spv::ExecutionModelIntersectionKHR:        return EShaderType::INTERSECTION;
            // case spv::ExecutionModelCallableKHR:            return EShaderType::CALLABLE;
            // case spv::ExecutionModelTaskEXT:                return EShaderType::TASK;
            // case spv::ExecutionModelMeshEXT:                return EShaderType::MESH;
            default:
                epi.stage = EShaderType::UNDEFINED;
            }
            comp.set_entry_point(entry.name, entry.execution_model);
            const auto resources = comp.get_shader_resources();
            const auto append = [&](const auto& source, auto& target)
            {
                for (const auto& variable : source)
                {
                    if (!comp.has_decoration(variable.id, spv::DecorationLocation))
                    {
                        continue;
                    }
                    const auto& type = comp.get_type(variable.type_id);
                    target.push_back(
                        {comp.get_decoration(variable.id, spv::DecorationLocation),
                         variable.name,
                         toVertexBase(type),
                         type.vecsize,
                         type.columns,
                         calcArraySize(comp, type)}
                    );
                }
                std::ranges::sort(target, {}, &ShaderIoVariable::location);
            };
            append(resources.stage_inputs, epi.inputs);
            append(resources.stage_outputs, epi.outputs);
            out.push_back(std::move(epi));
        }
        comp.set_entry_point(entries.front().name, entries.front().execution_model);
        return out;
    }

    static bool reflectFields(
        const sc::Compiler& compiler,
        const sc::SPIRType& type,
        std::string path,
        std::uint32_t offset,
        std::uint32_t inherited_stride,
        std::vector<ShaderMember>& result
    )
    {
        for (std::uint32_t i = 0; i < type.member_types.size(); ++i)
        {
            const auto& member = compiler.get_type(type.member_types[i]);
            const auto name = path + compiler.get_member_name(type.self, i);
            const auto base = offset + compiler.type_struct_member_offset(type, i);
            std::uint32_t count = 1, stride = inherited_stride;
            if (!member.array.empty())
            {
                if (member.array.size() != 1 || !member.array_size_literal[0] || member.array[0] == 0)
                {
                    return false;
                }
                count = member.array[0];
                stride = compiler.type_struct_member_array_stride(type, i);
            }
            for (std::uint32_t index = 0; index < count; ++index)
            {
                const auto item = name + (member.array.empty() ? "" : "[" + std::to_string(index) + "]");
                const auto at = base + (member.array.empty() ? 0 : index * stride);
                if (member.basetype == sc::SPIRType::Struct)
                {
                    if (!reflectFields(compiler, member, item + ".", at, stride, result))
                    {
                        return false;
                    }
                    continue;
                }
                const bool is_numeric = member.basetype == sc::SPIRType::Float ||
                                        member.basetype == sc::SPIRType::Int || member.basetype == sc::SPIRType::UInt;
                if (!is_numeric || member.width != 32 ||
                    compiler.has_member_decoration(type.self, i, spv::DecorationRowMajor))
                {
                    return false;
                }
                const auto scalar = member.basetype == sc::SPIRType::Float ? rdesc::EDataScalar::FLOAT
                                    : member.basetype == sc::SPIRType::Int ? rdesc::EDataScalar::SINT
                                                                           : rdesc::EDataScalar::UINT;
                const auto matrix_stride = member.columns > 1 ? compiler.type_struct_member_matrix_stride(type, i) : 0;
                result.push_back({item, scalar, at, member.vecsize, member.columns, stride, matrix_stride});
            }
        }
        return true;
    }

    static ShaderBlock reflectBlock(const sc::Compiler& compiler, const sc::SPIRType& type)
    {
        ShaderBlock result;
        result.size = static_cast<std::uint32_t>(compiler.get_declared_struct_size(type));
        const sc::SPIRType* record = &type;
        if (type.member_types.size() == 1)
        {
            const auto& member = compiler.get_type(type.member_types[0]);
            const bool is_record_array = member.array.size() == 1 && member.array_size_literal[0] &&
                                         member.array[0] == 0 && compiler.type_struct_member_offset(type, 0) == 0;
            if (is_record_array)
            {
                result.runtime_records = true;
                result.size = compiler.type_struct_member_array_stride(type, 0);
                record = &member;
            }
        }
        // Layout values come from SPIR-V decorations. Unsupported element shapes retain byte facts,
        // but have no member contract; consumers requiring typed members reject them at their boundary.
        if (result.runtime_records && record->basetype != sc::SPIRType::Struct)
        {
            const bool is_numeric = record->basetype == sc::SPIRType::Float || record->basetype == sc::SPIRType::Int ||
                                    record->basetype == sc::SPIRType::UInt;
            const bool is_supported = is_numeric && record->width == 32 &&
                                      !compiler.has_member_decoration(type.self, 0, spv::DecorationRowMajor);
            if (is_supported)
            {
                const auto scalar = record->basetype == sc::SPIRType::Float ? rdesc::EDataScalar::FLOAT
                                    : record->basetype == sc::SPIRType::Int ? rdesc::EDataScalar::SINT
                                                                            : rdesc::EDataScalar::UINT;
                const auto matrix_stride = record->columns > 1 ? compiler.type_struct_member_matrix_stride(type, 0) : 0;
                result.fields.push_back(
                    {compiler.get_member_name(type.self, 0),
                     scalar,
                     0,
                     record->vecsize,
                     record->columns,
                     0,
                     matrix_stride}
                );
            }
            return result;
        }
        if (!reflectFields(compiler, *record, "", 0, 0, result.fields))
        {
            result.fields.clear();
        }
        return result;
    }

    bool reflectSpirv(const void* bytes, size_t byte_count, ShaderInfo& out)
    {
        const uint32_t* words = reinterpret_cast<const uint32_t*>(bytes);
        size_t word_count = byte_count / sizeof(uint32_t);

        sc::Compiler comp(words, word_count);
        const auto res = comp.get_shader_resources();

        // entry points
        out = {};
        out.entry_points = detectEntry(comp);

        // UBO
        for (auto& r : res.uniform_buffers)
        {
            auto set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
            auto binding = comp.get_decoration(r.id, spv::DecorationBinding);
            const auto& t = comp.get_type(r.base_type_id);

            EDescriptorBindingInfo b{};
            b.set = set;
            b.binding = binding;
            b.name = comp.get_name(r.id);
            b.type = EDescriptorType::UNIFORM_BUFFER;
            b.count = calcArraySize(comp, comp.get_type(r.type_id));
            b.blockSize = static_cast<uint32_t>(comp.get_declared_struct_size(t));
            b.block = reflectBlock(comp, t);
            b.writable = false;
            pushBinding(out, b);
        }

        // SSBO
        for (auto& r : res.storage_buffers)
        {
            auto set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
            auto binding = comp.get_decoration(r.id, spv::DecorationBinding);
            const auto& t = comp.get_type(r.base_type_id);

            EDescriptorBindingInfo b{};
            b.set = set;
            b.binding = binding;
            b.name = comp.get_name(r.id);
            b.type = EDescriptorType::STORAGE_BUFFER;
            b.count = calcArraySize(comp, comp.get_type(r.type_id));
            b.blockSize = static_cast<uint32_t>(comp.get_declared_struct_size(t));
            b.block = reflectBlock(comp, t);
            b.writable = !comp.get_buffer_block_flags(r.id).get(spv::DecorationNonWritable);
            b.readable = !comp.get_buffer_block_flags(r.id).get(spv::DecorationNonReadable);
            pushBinding(out, b);
        }

        // Combined image samplers
        for (auto& r : res.sampled_images)
        {
            auto set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
            auto binding = comp.get_decoration(r.id, spv::DecorationBinding);
            const auto& t = comp.get_type(r.base_type_id);

            EDescriptorBindingInfo b{};
            b.set = set;
            b.binding = binding;
            b.name = comp.get_name(r.id);
            b.type = EDescriptorType::COMBINED_IMAGE_SAMPLER;
            b.count = calcArraySize(comp, comp.get_type(r.type_id));
            pushBinding(out, b);
        }

        // Separate images / samplers
        for (auto& r : res.separate_images)
        {
            auto set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
            auto binding = comp.get_decoration(r.id, spv::DecorationBinding);
            const auto& t = comp.get_type(r.base_type_id);
            EDescriptorBindingInfo b{};
            b.set = set;
            b.binding = binding;
            b.name = comp.get_name(r.id);
            b.type = EDescriptorType::SAMPLED_IMAGE;
            b.image_arrayed = t.image.arrayed;
            b.image_depth = t.image.depth;
            b.count = calcArraySize(comp, comp.get_type(r.type_id));
            pushBinding(out, b);
        }
        for (auto& r : res.separate_samplers)
        {
            auto set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
            auto binding = comp.get_decoration(r.id, spv::DecorationBinding);
            EDescriptorBindingInfo b{};
            b.set = set;
            b.binding = binding;
            b.name = comp.get_name(r.id);
            b.type = EDescriptorType::SAMPLER;
            b.count = calcArraySize(comp, comp.get_type(r.type_id));
            pushBinding(out, b);
        }

        // Storage images
        for (auto& r : res.storage_images)
        {
            auto set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
            auto binding = comp.get_decoration(r.id, spv::DecorationBinding);
            const auto& t = comp.get_type(r.base_type_id);
            EDescriptorBindingInfo b{};
            b.set = set;
            b.binding = binding;
            b.name = comp.get_name(r.id);
            b.type = EDescriptorType::STORAGE_IMAGE;
            b.image_arrayed = t.image.arrayed;
            b.image_depth = t.image.depth;
            b.count = calcArraySize(comp, comp.get_type(r.type_id));
            b.writable = isWritableStorage(comp, r.id);
            pushBinding(out, b);
        }

        // Input attachments
        for (auto& r : res.subpass_inputs)
        {
            auto set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
            auto binding = comp.get_decoration(r.id, spv::DecorationBinding);
            const auto& t = comp.get_type(r.base_type_id);
            EDescriptorBindingInfo b{};
            b.set = set;
            b.binding = binding;
            b.name = comp.get_name(r.id);
            b.type = EDescriptorType::INPUT_ATTACHMENT;
            b.count = calcArraySize(comp, comp.get_type(r.type_id));
            pushBinding(out, b);
        }

        // Acceleration structures
        for (auto& r : res.acceleration_structures)
        {
            auto set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
            auto binding = comp.get_decoration(r.id, spv::DecorationBinding);
            EDescriptorBindingInfo b{};
            b.set = set;
            b.binding = binding;
            b.name = comp.get_name(r.id);
            b.type = EDescriptorType::ACCELERATION_STRUCTURE;
            b.count = 1;
            pushBinding(out, b);
        }

        // Push constants
        for (auto& pc : res.push_constant_buffers)
        {
            const auto& t = comp.get_type(pc.base_type_id);
            PushConstantRangeInfo rng{};
            rng.offset = 0;
            rng.size = static_cast<uint32_t>(comp.get_declared_struct_size(t));
            rng.block = reflectBlock(comp, t);
            out.push_constants.push_back(rng);
        }

        // Specialization constants (fixed: read via SPIRConstant)
        for (auto& sc_info : comp.get_specialization_constants())
        {
            SpecConstantInfo s{};
            s.id = sc_info.id;
            s.constant_id = sc_info.constant_id;
            s.name = comp.get_name(sc_info.id);

            const auto& c = comp.get_constant(sc_info.id);
            const auto& t = comp.get_type(c.constant_type);

            s.vec_size = c.vector_size();
            s.columns = c.columns();

            using BT = sc::SPIRType::BaseType;
            auto& dv = s.default_value;
            dv.bit_width = t.width;

            switch (t.basetype)
            {
            case BT::Boolean:
                dv.kind = SpecDefaultValue::EKind::BOOL;
                dv.v.b8 = (c.scalar_u64() != 0) ? 1 : 0;
                break;
            case BT::Int:
                dv.kind = SpecDefaultValue::EKind::INT;
                if (t.width == 64)
                {
                    dv.v.i64 = c.scalar_i64();
                }
                else if (t.width == 16)
                {
                    dv.v.i64 = c.scalar_i16();
                }
                else if (t.width == 8)
                {
                    dv.v.i64 = c.scalar_i8();
                }
                else
                {
                    dv.v.i64 = c.scalar_i32();
                }
                break;
            case BT::UInt:
                dv.kind = SpecDefaultValue::EKind::UINT;
                if (t.width == 64)
                {
                    dv.v.u64 = c.scalar_u64();
                }
                else if (t.width == 16)
                {
                    dv.v.u64 = static_cast<uint16_t>(c.scalar_u16());
                }
                else if (t.width == 8)
                {
                    dv.v.u64 = static_cast<uint8_t>(c.scalar_u8());
                }
                else
                {
                    dv.v.u64 = static_cast<uint32_t>(c.scalar());
                }
                break;
            case BT::Half:
                dv.kind = SpecDefaultValue::EKind::FLOAT;
                dv.v.f64 = static_cast<double>(c.scalar_f16());
                break;
            case BT::Float:
                dv.kind = SpecDefaultValue::EKind::FLOAT;
                dv.v.f64 = static_cast<double>(c.scalar_f32());
                break;
            case BT::Double:
                dv.kind = SpecDefaultValue::EKind::DOUBLE;
                dv.v.f64 = c.scalar_f64();
                break;
            default:
                dv.kind = SpecDefaultValue::EKind::UNKNOWN;
                dv.v.u64 = 0;
                break;
            }

            out.spec_constants.push_back(std::move(s));
        }

        // First check whether there's a vertex entry point
        bool has_vertex = false;
        std::string vertex_entry;
        for (auto& ep : out.entry_points)
        {
            if (ep.stage == EShaderType::VERTEX)
            {
                has_vertex = true;
                // Prefer the entry point named "main"; fall back to the first one found
                if (vertex_entry.empty() || ep.name == "main")
                {
                    vertex_entry = ep.name;
                }
            }
        }

        if (has_vertex)
        {
            // To avoid picking up the wrong interface when there are multiple
            // entry points, fetch resources again specifically for the vertex entry
            sc::Compiler comp_vi(words, word_count);
            if (!vertex_entry.empty())
            {
                comp_vi.set_entry_point(vertex_entry, spv::ExecutionModelVertex);
            }

            const auto res_vi = comp_vi.get_shader_resources();

            for (auto& a : res_vi.stage_inputs)
            {
                // Skip built-in variables (e.g. gl_VertexIndex / gl_InstanceIndex)
                if (comp_vi.has_decoration(a.id, spv::DecorationBuiltIn))
                {
                    continue;
                }

                ShaderIoVariable vi{};
                // Location (required)
                vi.location = comp_vi.get_decoration(a.id, spv::DecorationLocation);
                // Name (for readability)
                vi.name = comp_vi.get_name(a.id);

                // Type info
                const auto& t = comp_vi.get_type(a.base_type_id);
                vi.base = toVertexBase(t); // Bool / Int / UInt / Float / Double / Unknown
                vi.vec_size = t.vecsize;   // 1,2,3,4
                vi.columns = t.columns;    // matrix column count; 1 for scalars/vectors
                vi.array_size =
                    t.array.empty() ? 1u : static_cast<uint32_t>(t.array[0]); // only the outermost dimension

                out.vertex_inputs.push_back(std::move(vi));
            }

            // Sort by location ascending, so upstream code can generate
            // VkShaderIoVariableDescription directly
            std::sort(
                out.vertex_inputs.begin(),
                out.vertex_inputs.end(),
                [](const auto& lhs, const auto& rhs) { return lhs.location < rhs.location; }
            );
        }

        std::uint32_t stages = 0;
        for (const auto& entry : out.entry_points)
        {
            stages |= entry.stage == EShaderType::VERTEX     ? 1u
                      : entry.stage == EShaderType::FRAGMENT ? 2u
                      : entry.stage == EShaderType::COMPUTE  ? 4u
                                                             : 0u;
        }
        for (auto& set : out.sets)
        {
            for (auto& binding : set.bindings)
            {
                binding.stages = stages;
            }
        }
        for (auto& range : out.push_constants)
        {
            range.stages = stages;
        }
        return true;
    }
} // namespace lux::toolchain
