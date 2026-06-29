#pragma once
#include <lux/cxx/container/SparseSet.hpp>
#include <lux/engine/render/pipeline/VertexLayout.hpp>

namespace lux::render
{
    class VertexLayoutRegistry
    {
    public:
        VertexLayoutId registerLayout(const VertexLayout& desc)
        {
            VertexLayoutId id = static_cast<VertexLayoutId>(layouts_.size());
            layouts_.insert(id, desc);
            return id;
        }

        VertexLayout& fetchLayout(VertexLayoutId id)
        {
            return layouts_.at(id);
        }

        const VertexLayout& fetchLayout(VertexLayoutId id) const
        {
            return layouts_.at(id);
        }

        bool hasLayout(VertexLayoutId id) const 
        { 
            return layouts_.contains(id);
        }
        
        bool removeLayout(VertexLayoutId id)
        {
            return layouts_.erase(id);
        }

    private:

        using LayoutMap = lux::cxx::SparseSet<VertexLayoutId, VertexLayout>;
        LayoutMap layouts_;
    };
}