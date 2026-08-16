#version 450

// Test-only Graph-family fragment. The production vertex stage supplies the
// instance tint at location 10; keeping this shader descriptor-free makes the
// semantic probe independent of material-layout implementation details while
// still exercising the real mesh pipeline, raster state and instance stream.
layout(location = 10) in flat vec4 vInstanceTint;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vInstanceTint;
}
