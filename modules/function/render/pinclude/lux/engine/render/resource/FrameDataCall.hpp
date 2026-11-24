#pragma once

#include <lux/engine/render/resource/StorageMeta.hpp>

namespace lux::render::detail
{
    // Qualified on the Server owner; the command borrow covers all updates in this packet.
    struct FrameDataCall final
    {
        ResourceInstanceId id;
        void* owner;
        ResourceRegistration::ApplyFrame apply;
        std::size_t first_column;
        bool per_run;
        std::size_t frame_resource = SIZE_MAX;
    };
}
