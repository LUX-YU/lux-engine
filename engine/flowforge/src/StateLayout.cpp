#include <lux/engine/flowforge/graph/StateLayout.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>

#include <algorithm>
#include <cstring>

namespace lux::flowforge
{
    namespace
    {
        bool isScalarBase(const lux::meta::RefType& rt)
        {
            using lux::meta::EBaseType;
            switch (static_cast<EBaseType>(rt.qtype.base)) {
                case EBaseType::Bool:
                case EBaseType::Int8:
                case EBaseType::Uint8:
                case EBaseType::Int16:
                case EBaseType::Uint16:
                case EBaseType::Int32:
                case EBaseType::Uint32:
                case EBaseType::Int64:
                case EBaseType::Uint64:
                case EBaseType::Float:
                case EBaseType::Double:
                    return true;
                default:
                    return false;
            }
        }

        bool isPointerQual(const lux::meta::RefType& rt)
        {
            using lux::meta::ETypeQual;
            switch (static_cast<ETypeQual>(rt.qtype.qual)) {
                case ETypeQual::Ptr:
                case ETypeQual::PtrToConst:
                case ETypeQual::ConstPtr:
                case ETypeQual::ConstPtrToConst:
                    return true;
                default:
                    return false;
            }
        }

        // FNV-1a. LLVM's hashing is unavailable in this module, and the hash
        // only needs to be stable across builds and collision-poor enough
        // for layout-identity compares.
        void hashBytes(uint64_t& h, const void* data, size_t n)
        {
            const auto* p = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < n; ++i) {
                h ^= p[i];
                h *= 0x100000001b3ULL;
            }
        }
    }

    StateLayout computeStateLayout(const FlowGraph& graph, std::string* error_out)
    {
        const auto fail = [&](std::string msg) {
            if (error_out) *error_out = std::move(msg);
            return StateLayout{};
        };
        if (error_out) error_out->clear();

        // Deterministic field order: by stable variable id. Declaration
        // order can be shuffled by editor deletes and re-adds; ids cannot.
        std::vector<const FlowGraph::GraphVariable*> vars;
        vars.reserve(graph.variables().size());
        for (const auto& v : graph.variables()) vars.push_back(&v);
        std::sort(vars.begin(), vars.end(),
                  [](const auto* a, const auto* b) { return a->id < b->id; });

        StateLayout layout;
        uint64_t h = 0xcbf29ce484222325ULL;   // FNV offset basis

        for (const auto* v : vars) {
            if (!v->type)
                return fail("graph variable '" + v->name + "' has no type");
            if (isPointerQual(*v->type) || !isScalarBase(*v->type))
                return fail("graph variable '" + v->name
                            + "': only scalar graph variables are supported yet");
            if (!v->default_value.isValid())
                return fail("graph variable '" + v->name
                            + "' has no valid default value");
            if (v->default_value.type()->hash != v->type->hash)
                return fail("graph variable '" + v->name
                            + "': default value type does not match the "
                              "variable's declared type");

            const uint32_t sz = v->type->size;
            if (sz == 0 || sz > 8 || (sz & (sz - 1)) != 0)
                return fail("graph variable '" + v->name
                            + "' has an unsupported storage size");
            const uint32_t al  = sz;   // scalars: natural alignment == size
            const uint32_t off = (layout.size + al - 1) & ~(al - 1);

            layout.fields.push_back(StateFieldLayout{v->id, v->name, v->type, off});
            layout.size  = off + sz;
            layout.align = std::max(layout.align, al);

            hashBytes(h, v->name.data(), v->name.size());
            hashBytes(h, &v->type->hash, sizeof(v->type->hash));
            hashBytes(h, &off, sizeof(off));
        }

        // Round the block up to its alignment (array-of-instances math).
        layout.size = (layout.size + layout.align - 1) & ~(layout.align - 1);
        hashBytes(h, &layout.size, sizeof(layout.size));
        layout.hash = layout.fields.empty() ? 0 : h;

        layout.defaults.assign(layout.size, std::byte{0});
        for (const auto& f : layout.fields) {
            const auto* v = graph.findVariable(f.var_id);
            std::memcpy(layout.defaults.data() + f.offset,
                        v->default_value.data(), f.type->size);
        }
        return layout;
    }
}
