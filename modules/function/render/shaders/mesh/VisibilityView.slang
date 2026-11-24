// GPU layout shared by visibility preparation, culling, mesh raster and shadow sampling.
struct VisibilityView
{
    mat4 world_to_clip;
    vec4 planes[6];
    uvec4 flags;
};
