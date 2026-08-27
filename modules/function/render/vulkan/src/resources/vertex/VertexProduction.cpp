/**
 * @file VertexProduction.cpp
 */

#include <lux/engine/render/resources/vertex/VertexProduction.hpp>

#include <algorithm>

namespace lux::render
{
    const char* vertexProductRgName(EVertexProductKind kind) noexcept
    {
        switch (kind)
        {
        // NOTE: "SkinnedVertexPool" intentionally matches the literal that the
        // skinning producer/consumer used before this refactor, so the RG name
        // is unchanged across the migration — only the way it's referenced
        // (typed enum vs raw string) changes.
        case EVertexProductKind::Skinning:
            return "SkinnedVertexPool";
        case EVertexProductKind::Morph:
            return "MorphVertexPool";
        case EVertexProductKind::Cloth:
            return "ClothVertexPool";
        }
        return "UnknownVertexPool";
    }

    void VertexProductionRegistry::publish(EVertexProductKind kind)
    {
        for (auto& r : records_)
        {
            if (r.kind == kind)
            {
                r.rg_buffer_name = vertexProductRgName(kind); // refresh, no dup
                return;
            }
        }
        records_.push_back(VertexProductionRecord{kind, vertexProductRgName(kind)});
    }

    void VertexProductionRegistry::unpublish(EVertexProductKind kind) noexcept
    {
        records_.erase(
            std::remove_if(
                records_.begin(),
                records_.end(),
                [kind](const VertexProductionRecord& r) { return r.kind == kind; }
            ),
            records_.end()
        );
    }

} // namespace lux::render
