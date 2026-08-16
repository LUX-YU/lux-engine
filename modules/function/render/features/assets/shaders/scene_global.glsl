#ifndef LUX_SCENE_GLOBAL_GLSL
#define LUX_SCENE_GLOBAL_GLSL

// Must match SceneGlobalGpuData in SceneResources.hpp.
struct SceneGlobalGpuData {
    float time_sec;
    float delta_time;
    uint frame_number;
    float pad0;
};

#endif
