#version 450

// =============================================================================
// LOD / Splatting point cloud vertex shader
//
// Vertex format: GpuPointVertex (16 bytes, same as pointcloud_simple.vert)
//   - vec3 position  (12 bytes, location 0)
//   - uint packed_attr (4 bytes, location 1)
//
// Key difference from pointcloud_simple.vert:
//   gl_PointSize is computed from a world-space point radius so that
//   screen-space coverage remains constant regardless of depth.
//   This implements perspective-correct LOD point sizing.
//
// Formula:
//   gl_PointSize = point_size_world / clip.w * viewport_height * |proj[1][1]| / 2
//
// where:
//   point_size_world : desired world-space diameter of the point sphere (PC)
//   clip.w           : view-space linear depth (homogeneous W = -view_z)
//   viewport_height  : screen height in pixels (uViewport.y)
//   proj[1][1]       : focal-y of the projection matrix (= cot(fovY/2))
// =============================================================================

// ========== Vertex Input (GpuPointVertex, 16 bytes) ==========
layout(location = 0) in vec3 inPosition;
layout(location = 1) in uint inPackedAttr;

// ========== SET 0: View (std140, same as fr.vert / pointcloud_simple.vert) =====
struct ViewGpuData {
    mat4 view;
    mat4 proj;
    mat4 inv_view;
    mat4 inv_proj;
    vec4 cam_pos;
    vec4 viewport;
};
layout(set = 0, binding = 1, std430) readonly buffer ViewBuffer {
    ViewGpuData views[];
} uViews;

// ========== Push Constant (12 bytes) ==========
layout(push_constant) uniform PC {
    uint  scene_index;    ///< SceneGlobalBuffer slot
    uint  view_index;      ///< ViewBuffer slot
    float point_size_world;  ///< World-space point diameter (meters)
    float min_size;          ///< Minimum screen-space size clamp (pixels)
    float max_size;          ///< Maximum screen-space size clamp (pixels)
} uPC;

// ========== Output to fragment shader ==========
layout(location = 0) out vec4 vColor;

void main()
{
    vec4 clip = uViews.views[uPC.view_index].proj * uViews.views[uPC.view_index].view * vec4(inPosition, 1.0);
    gl_Position = clip;

    // Unpack RGBA from packed attribute
    float r = float((inPackedAttr >>  0u) & 0xFFu) / 255.0;
    float g = float((inPackedAttr >>  8u) & 0xFFu) / 255.0;
    float b = float((inPackedAttr >> 16u) & 0xFFu) / 255.0;
    vColor = vec4(r, g, b, 1.0);

    // Perspective-correct point size:
    //   At depth d in view space, a world-space sphere of radius r covers:
    //   pixels = r / d * focal_y * viewport_height / 2
    // clip.w == -view_z (positive for objects in front of camera)
    float view_depth = max(clip.w, 0.001);
    float focal_y    = abs(uViews.views[uPC.view_index].proj[1][1]);
    float proj_scale = uViews.views[uPC.view_index].viewport.y * focal_y * 0.5;
    gl_PointSize = clamp(uPC.point_size_world / view_depth * proj_scale,
                         uPC.min_size, uPC.max_size);
}
