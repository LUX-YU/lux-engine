#version 450
// =============================================================================
// particle.vert  — GPU particle billboard vertex shader
//
// Each particle occupies 6 vertices (2 triangles).  No VBO is needed:
//   gl_InstanceIndex  = particle index in the SSBO
//   gl_VertexIndex    = 0..5, used to generate billboard quad corners
//
// Set 0 (binding 0) : Scene UBO     — same layout as fr.vert
// Set 5 (binding 0) : Particle SSBO — ParticleGpu[max_particles] (standard Particle slot)
// =============================================================================

// ---- View UBO (set 0 binding 1) ----
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

// ====== Shared Push Constants (scene/view indices, offset 0) ======
layout(push_constant) uniform SharedPC {
    uint scene_index;
    uint view_index;
} uPC;

// ---- Particle data struct (must match ParticleGpu in ParticleResources.hpp) ----
struct ParticleGpu {
    vec3  position;       // world-space position
    float lifetime;       // remaining lifetime in seconds (< 0 = dead)
    vec3  velocity;       // world-space velocity
    float max_lifetime;   // initial / maximum lifetime (for normalizing alpha)
    vec4  color;          // rgba tint
    float size;           // world-space billboard half-extent
    uint  flags;          // bit0 = alive
    vec2  _pad;
};

layout(set = 5, binding = 0, std430) readonly buffer ParticleSSBO {
    ParticleGpu particles[];
} uParticles;

// ---- Outputs to fragment shader ----
layout(location = 0) out vec4  vColor;
layout(location = 1) out vec2  vBillboardUV;    // -1..1 in billboard space
layout(location = 2) out float vLifetimeFrac;   // remaining/max (0=dead, 1=just born)

// Quad corner offsets in billboard space (2 tris, CCW)
const vec2 kCorners[6] = vec2[6](
    vec2(-1.0, -1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0,  1.0)
);

void main()
{
    uint  pid    = uint(gl_InstanceIndex);
    uint  vid    = uint(gl_VertexIndex);
    ParticleGpu p = uParticles.particles[pid];

    // Dead particles: clip off-screen (behind near plane so no FS work wasted).
    if (p.lifetime < 0.0 || (p.flags & 1u) == 0u) {
        gl_Position   = vec4(0.0, 0.0, -2.0, 1.0);   // z < 0 → behind camera
        vColor        = vec4(0.0);
        vBillboardUV  = vec2(0.0);
        vLifetimeFrac = 0.0;
        return;
    }

    // Camera right and up in world space (extract from inverse-view columns).
    // inv_view[0].xyz = camera right,  inv_view[1].xyz = camera up.
    vec3 camRight = vec3(uViews.views[uPC.view_index].inv_view[0][0], uViews.views[uPC.view_index].inv_view[1][0], uViews.views[uPC.view_index].inv_view[2][0]);
    vec3 camUp    = vec3(uViews.views[uPC.view_index].inv_view[0][1], uViews.views[uPC.view_index].inv_view[1][1], uViews.views[uPC.view_index].inv_view[2][1]);

    // Build world-space billboard corner
    vec2  corner      = kCorners[vid];
    vec3  worldPos    = p.position
                      + camRight * (corner.x * p.size)
                      + camUp    * (corner.y * p.size);

    gl_Position   = uViews.views[uPC.view_index].proj * uViews.views[uPC.view_index].view * vec4(worldPos, 1.0);
    vColor        = p.color;
    vBillboardUV  = corner;
    vLifetimeFrac = clamp(p.lifetime / max(p.max_lifetime, 0.001), 0.0, 1.0);
}
