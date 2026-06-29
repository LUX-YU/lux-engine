#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/render/FrameStamp.hpp>

#include <cstdint>

namespace lux::render
{

enum class EUploadPhase : uint8_t
{
    PreUpload = 0,
    Upload = 1,
    PostUpload = 2,
    Count
};

class LUX_FUNCTION_PUBLIC IGlobalFrameService
{
public:
    virtual ~IGlobalFrameService() = default;
    virtual void onBeginFrame(const FrameStamp& /*stamp*/) {}
    virtual void onEndFrame(const FrameStamp& /*stamp*/) {}
    virtual void onViewDestroyed(uint32_t /*view_id*/) {}
    virtual void onSceneViewDestroyed(uint32_t /*scene_key*/, uint32_t view_id)
    {
        onViewDestroyed(view_id);
    }
};

class LUX_FUNCTION_PUBLIC ISceneFrameService
{
public:
    virtual ~ISceneFrameService() = default;
    virtual void onBeginFrame(const FrameStamp& /*stamp*/) {}
    virtual void onEndFrame(const FrameStamp& /*stamp*/) {}
    virtual void onViewDestroyed(uint32_t /*view_id*/) {}
    virtual void onSceneViewDestroyed(uint32_t /*scene_key*/, uint32_t view_id)
    {
        onViewDestroyed(view_id);
    }
};

} // namespace lux::render
