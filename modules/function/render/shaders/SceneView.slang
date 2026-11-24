// Matches ViewGpuParameters. The descriptor's first element selects the ViewId row in its device/FIF page.
struct SceneViewGpu
{
    column_major float4x4 view;
    column_major float4x4 projection;
    column_major float4x4 inverse_view_projection;
    float4 position_near;
    float4 direction_far;
    float4 viewport;
    float4 background;
};
