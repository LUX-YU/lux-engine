#version 450

// ====== View UBO (SET=0, BINDING=1) ======
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

// ====== Push Constants ======
layout(push_constant) uniform GridPC {
    uint  scene_index;
    uint  view_index;
    float planeY;    // Grid plane Y height
    float cellSize;  // Base cell size (e.g. 1.0)
    float linePx;    // Target line width (pixels), recommended 1.5 ~ 2.0
    float fadeDist;  // Visual "infinity" distance control (recommended 1000.0+)
    float holeRatio; // (Not directly used in this algorithm, kept for compatibility)
} uPC;

layout(location = 0) noperspective in vec2 v_ndc;
layout(location = 0) out vec4 outColor;

// --- Create ray
void makeRay(in vec2 ndc, out vec3 ro, out vec3 rd) {
    vec4 v = uViews.views[uPC.view_index].inv_proj * vec4(ndc, 1.0, 1.0);
    v /= v.w;
    rd = normalize(mat3(uViews.views[uPC.view_index].inv_view) * v.xyz);
    ro = uViews.views[uPC.view_index].cam_pos.xyz;
}

// --- Ray-plane intersection
bool rayPlane(vec3 ro, vec3 rd, float height, out vec3 hitPos) {
    if (abs(rd.y) < 1e-6) return false;
    float t = (height - ro.y) / rd.y;
    if (t < 0.0) return false;
    hitPos = ro + t * rd;
    return true;
}

// --- Compute raw depth (for writing to Depth Buffer)
float computeDepth(vec3 pos) {
    vec4 clip = uViews.views[uPC.view_index].proj * uViews.views[uPC.view_index].view * vec4(pos, 1.0);
    return (clip.z / clip.w); // Vulkan depth range [0, 1] (with VK_KHR_maintenance1 or standard viewport transform)
    // If using OpenGL convention [-1, 1] mapped to Vulkan, may need z*0.5 + 0.5
}

// --- Core grid function: Pristine Grid algorithm
// coord: world coordinates divided by grid size
// derivative: fwidth(coord)
float gridFactor(vec2 coord, vec2 derivative, float lineWidthPixels) {
    vec2 grid = abs(fract(coord - 0.5) - 0.5);
    // Convert line width from pixel units to current grid coordinate units
    // derivative is "grid coordinate change per pixel"
    vec2 lineAA = derivative * lineWidthPixels; 
    
    // 1.0 - smoothstep(...) is the standard anti-aliased hard edge
    // But to handle far-away sub-pixel issues, we optimize as follows:
    // When lineAA (line width in grid space) increases, still draw 1px wide but reduce opacity
    vec2 lines = 1.0 - min(grid / lineAA, 1.0);
    
    // Choose the stronger line between X and Z directions
    float alpha = max(lines.x, lines.y);
    
    return alpha;
}

void main() {
    // 1. Ray intersection
    vec3 ro, rd;
    makeRay(v_ndc, ro, rd);
    
    vec3 wp;
    if (!rayPlane(ro, rd, uPC.planeY, wp)) discard;

    // 2. Base data preparation
    // Use fwidth for screen-space derivatives, key for anti-aliasing
    // To prevent flickering from overly large derivatives at extreme distances, a small clamp can be added
    vec2 uv = wp.xz;
    vec2 derivative = fwidth(uv); 
    
    // 3. Calculate two grid levels (LOD)
    // Level 1: Base small grid (Base Grid)
    float lod0_size = max(uPC.cellSize, 1e-5);
    vec2 coord0 = uv / lod0_size;
    vec2 deriv0 = derivative / lod0_size;
    float grid0 = gridFactor(coord0, deriv0, uPC.linePx);
    
    // Level 2: Major grid (e.g. every 10 small cells grouped)
    float lod1_size = lod0_size * 10.0;
    vec2 coord1 = uv / lod1_size;
    vec2 deriv1 = derivative / lod1_size;
    float grid1 = gridFactor(coord1, deriv1, uPC.linePx * 2.0); // Slightly wider lines for major grid

    // 4. Calculate grid blending weight
    // When the small grid becomes too dense (less than 1 pixel spacing), fade it out and keep only the major grid
    // deriv0 is the number of small cells per pixel. If > 1.0, multiple cells fit in one pixel and must fade out.
    // We fade between 0.5 (2 pixels per cell) and 1.0 (1 pixel per cell)
    float LodFade = min(1.0, max(deriv0.x, deriv0.y) * 2.0); 
    LodFade = smoothstep(0.0, 1.0, LodFade); 

    // Blend grid colors (customizable)
    vec3 cMinor = vec3(0.5); // Minor grid color (gray)
    vec3 cMajor = vec3(0.8); // Major grid color (light gray)
    
    float alphaGrid = mix(grid0, grid1, LodFade); // Geometric blend
    vec3 colorGrid  = mix(cMinor, cMajor, LodFade); // Color blend
    // Ensure major grid always covers on top (optional, makes major grid clearer)
    alphaGrid = max(grid0 * (1.0 - LodFade), grid1); 

    // 5. Draw coordinate axes (X-axis red, Z-axis blue)
    // Axis lines are slightly wider
    float axisWidth = uPC.linePx * 2.5;
    // Calculate distance from point to axis (only considering X and Z axes)
    float distX = abs(wp.z); // Z=0 is the X-axis
    float distZ = abs(wp.x); // X=0 is the Z-axis
    
    // Calculate axis alpha (similar to Grid, but no looping needed)
    // Dividing by derivative.y because wp.z corresponds to screen Y derivative, simplified using length or corresponding component
    float axisAlphaX = 1.0 - min(distX / (derivative.y * axisWidth), 1.0);
    float axisAlphaZ = 1.0 - min(distZ / (derivative.x * axisWidth), 1.0);
    
    bool isX = axisAlphaX > 0.0;
    bool isZ = axisAlphaZ > 0.0;

    vec3 axisColor = vec3(0.0);
    float axisAlpha = 0.0;

    if (isX) {
        axisColor = vec3(0.8, 0.1, 0.1); // Red
        axisAlpha = axisAlphaX;
    }
    if (isZ) {
        // If overlapping (origin), blend or prioritize Z
        vec3 cZ = vec3(0.1, 0.1, 0.8); // Blue
        axisColor = isX ? mix(axisColor, cZ, 0.5) : cZ; 
        axisAlpha = max(axisAlpha, axisAlphaZ);
    }

    // 6. Combine axes and grid
    // Axes on top layer
    vec3 finalColor = mix(colorGrid, axisColor, axisAlpha);
    float finalAlpha = max(alphaGrid, axisAlpha);

    // 7. Distance fade
    // Use exponential decay to simulate fog, more natural than linear clamp
    float dist = distance(ro, wp);
    // 50.0 is the fade softness, FadeDist is the reference point for full disappearance
    float fadeFactor = exp(-max(dist - uPC.fadeDist * 0.5, 0.0) / (uPC.fadeDist * 0.2));
    
    // Grazing angle fade (prevents artifacts at the horizon)
    float grazingFade = smoothstep(0.0, 0.1, abs(rd.y));
    
    finalAlpha *= fadeFactor * grazingFade;

    // 8. Output
    if (finalAlpha < 0.01) discard;

    // Premultiplied alpha (if Blend State is SrcAlpha, OneMinusSrcAlpha, no need to multiply color)
    // Assuming Blend State: SrcAlpha, OneMinusSrcAlpha
    outColor = vec4(finalColor, finalAlpha);

    // Bias the grid slightly further from camera (Vulkan: larger depth = farther)
    // so any coplanar opaque surface — e.g. the editor floor mesh at Y=0 — wins
    // the LESS_OR_EQUAL test deterministically. Rasterizer depth-bias is a no-op
    // when the shader writes gl_FragDepth, so we add the offset here. 1e-5 is
    // ~0.1× a 24-bit depth step near the far plane — invisible, but enough to
    // break the FP-precision tie that otherwise flickers per frame.
    gl_FragDepth = computeDepth(wp) + 1e-5;
}