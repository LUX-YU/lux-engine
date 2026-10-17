#include <lux/engine/render/resource/Instance.hpp>
#include <lux/cxx/core/CheckedArithmetic.hpp>

namespace lux::render
{
    RenderResult<std::size_t> CommandValueTraits<TransformBatch>::retainedBytes(const TransformBatch& value) noexcept
    {
        const auto bytes = cxx::checkedMul(value.updates.capacity(), sizeof(TransformUpdate));
        if (!bytes) return cxx::unexpected(RenderError{kProtocolContract});
        const auto total = cxx::checkedAdd(sizeof(value), *bytes);
        if (!total) return cxx::unexpected(RenderError{kProtocolContract});
        return *total;
    }
}
