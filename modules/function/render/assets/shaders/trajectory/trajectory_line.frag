#version 450

// =============================================================================
// Trajectory Line fragment shader
//
// Outputs the interpolated vertex color with alpha.
// Future: can use vTime for fading effects along the trajectory.
// =============================================================================

layout(location = 0) in vec4  vColor;
layout(location = 1) in float vTime;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vColor;
}
