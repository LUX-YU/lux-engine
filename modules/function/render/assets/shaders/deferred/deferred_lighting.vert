#version 450
// =========================================================================
//  deferred_lighting.vert — Fullscreen triangle vertex shader
//
//  Generates a single triangle that covers the entire screen using
//  gl_VertexIndex (no vertex input required). Draw with 3 vertices.
// =========================================================================

layout(location = 0) out vec2 vUV;

void main()
{
    // Fullscreen triangle: vertices 0,1,2 produce
    //   (-1,-1), (3,-1), (-1,3) in clip space
    vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
}
