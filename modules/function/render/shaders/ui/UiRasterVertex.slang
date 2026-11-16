#include "../Common.slang"
static vec2 position;
static vec2 uv;
static vec4 vertex_color;

struct Parameters
{
    vec2 scale;
    vec2 translation;
    uint source_index;
};

[[vk::push_constant]] ConstantBuffer<Parameters> parameters : register(b0, space255);
static vec2 texture_uv;
static vec4 color;

void luxMain()
{
    gl_Position = vec4(position * parameters.scale + parameters.translation, 0, 1);
    texture_uv = uv;
    vec3 encoded = vertex_color.rgb;
    color = vec4(
        mix(pow((encoded + 0.055) / 1.055, vec3(2.4)), encoded / 12.92, lessThanEqual(encoded, vec3(0.04045))),
        vertex_color.a
    );
}

struct LuxInput
{
    [[vk::location(0)]] vec2 position : TEXCOORD0;
    [[vk::location(1)]] vec2 uv : TEXCOORD1;
    [[vk::location(2)]] vec4 vertex_color : TEXCOORD2;
    uint vertexID : SV_VertexID;
    uint instanceID : SV_InstanceID;
#ifndef LUX_TARGET_DXIL
    uint baseInstance : SV_StartInstanceLocation;
#endif
};

struct LuxOutput
{
    [[vk::location(0)]] vec2 texture_uv : TEXCOORD0;
    [[vk::location(1)]] vec4 color : TEXCOORD1;
    float4 position : SV_Position;
};
[shader("vertex")] LuxOutput main(LuxInput input)
{
    position = input.position;
    uv = input.uv;
    vertex_color = input.vertex_color;
    gl_VertexIndex = input.vertexID;
    gl_InstanceIndex = input.instanceID;
#ifndef LUX_TARGET_DXIL
    gl_InstanceIndex += input.baseInstance;
#endif
    luxMain();
    LuxOutput output;
    output.texture_uv = texture_uv;
    output.color = color;
    output.position = gl_Position;
#ifdef LUX_TARGET_DXIL
    output.position.y = -output.position.y;
#endif
    return output;
}
