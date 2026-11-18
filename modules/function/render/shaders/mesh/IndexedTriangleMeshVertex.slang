#include "../Common.slang"

// DXIL assigns signature registers from the declared varyings. Its vertex variants export
// the same sorted TEXCOORD subset as the selected material fragment shader.
#ifndef MESH_VARYINGS
    #define MESH_VARYINGS 0
    #ifndef DEPTH_ONLY
        #undef MESH_VARYINGS
        #define MESH_VARYINGS (1 | 8 | MESH_NORMAL | MESH_UV | MESH_TANGENTS)
        #ifdef VERTEX_NORMALS
            #define MESH_NORMAL 2
        #else
            #define MESH_NORMAL 0
        #endif
        #ifdef VERTEX_NO_UV
            #define MESH_UV 0
        #else
            #define MESH_UV 4
        #endif
        #ifdef VERTEX_TANGENTS
            #define MESH_TANGENTS (16 | 32)
        #else
            #define MESH_TANGENTS 0
        #endif
    #endif
#endif

#ifdef PAGED_INSTANCES
uint64_t luxAddress(uint2 words)
{
    return uint64_t(words.x) | (uint64_t(words.y) << 32);
}
#endif
#ifdef PAGED_INSTANCES
#endif
static vec3 position;
#ifndef DEPTH_ONLY
#ifndef VERTEX_NO_UV
static vec2 uv;
static vec2 vUV;
#endif
static vec3 vWorldPos;
#ifdef VERTEX_NORMALS
static vec3 normal;
static vec3 vWorldNormal;
#endif
#ifdef VERTEX_TANGENTS
static vec4 tangent;
static vec4 vWorldTangent;
static vec3 vWorldBitangent;
#endif
static uvec2 vMaterial;
#endif
struct Transform
{
    mat4 model;
};

struct Property
{
    uint mesh_index;
    uint material_buffer;
    uint material_record;
    uint section_index;
    uint render_flags;
};

struct Word
{
    uint value;
};
#ifdef PAGED_INSTANCES
struct Address
{
    uvec2 value;
};

struct PageAddresses
{
    uvec2 transforms;
    uvec2 properties;
    uvec2 alive;
};

[[vk::binding(7, 3)]] StructuredBuffer<Address> transforms : register(t7, space3);
[[vk::binding(8, 3)]] StructuredBuffer<Address> properties : register(t8, space3);
[[vk::binding(9, 3)]] StructuredBuffer<Address> alive : register(t9, space3);
#else
[[vk::binding(7, 3)]] StructuredBuffer<Transform> transforms : register(t7, space3);
[[vk::binding(8, 3)]] StructuredBuffer<Property> properties : register(t8, space3);
[[vk::binding(9, 3)]] StructuredBuffer<Word> alive : register(t9, space3);
#endif
[[vk::binding(6, 2)]] StructuredBuffer<Word> members : register(t6, space3);

struct Camera
{
    mat4 view;
    mat4 projection;
};

[[vk::push_constant]] ConstantBuffer<Camera> camera : register(b0, space255);

void luxMain()
{
    uint slot = members[5u + uint(gl_InstanceIndex)].value;
#ifdef PAGED_INSTANCES
    uint root = slot >> 23;
    uint leaf = (slot >> 14) & 511;
    uint local = slot & 16383;
    mat4 model =
        ((Transform*)luxAddress(((PageAddresses*)luxAddress(transforms[root].value))[leaf].transforms))[local].model;
    Property property =
        ((Property*)luxAddress(((PageAddresses*)luxAddress(properties[root].value))[leaf].properties))[local];
    uint present = ((Word*)luxAddress(((PageAddresses*)luxAddress(alive[root].value))[leaf].alive))[local].value;
#else
    mat4 model = transforms[slot].model;
    Property property = properties[slot];
    uint present = alive[slot].value;
#endif
    float orientation = dot(transpose(model)[0].xyz, cross(transpose(model)[1].xyz, transpose(model)[2].xyz));
#ifdef REFLECTED_INSTANCES
    bool selected = orientation < 0;
#else
    bool selected = orientation > 0;
#endif
    // The all-candidate path uses the same two raster orientations without a CPU transform scan.
    // GPU visibility already supplies a matching stream; discarded instances produce no primitives.
#ifdef SHADOW_PASS
    const uint required = 3u;
#else
    const uint required = 1u;
#endif
    bool permitted = (property.render_flags & required) == required;
    gl_Position = present != 0 && selected && permitted
                      ? mul(camera.projection, mul(camera.view, mul(model, vec4(position, 1))))
                      : vec4(2, 2, 2, 1);
#ifndef DEPTH_ONLY
#ifndef VERTEX_NO_UV
    vUV = uv;
#endif
    vWorldPos = (mul(model, vec4(position, 1.0))).xyz;
#ifdef VERTEX_NORMALS
    mat3 linear_matrix = (float3x3)model;
    // Legacy cofactor construction, corrected for negative determinants and tangent-vector transforms.
    mat3 cofactor = transpose(float3x3(
        cross(transpose(linear_matrix)[1], transpose(linear_matrix)[2]),
        cross(transpose(linear_matrix)[2], transpose(linear_matrix)[0]),
        cross(transpose(linear_matrix)[0], transpose(linear_matrix)[1])
    ));
    float handedness = sign(dot(transpose(linear_matrix)[0], transpose(cofactor)[0]));
    vec3 n = normalize(mul(cofactor, normal) * handedness);
    vWorldNormal = n;
#ifdef VERTEX_TANGENTS
    vec3 t = mul(linear_matrix, tangent.xyz);
    float tangent_length_squared = dot(t, t);
    t -= n * dot(n, t);
    if (dot(t, t) <= tangent_length_squared * 1e-12)
    {
        // Missing/degenerate UV tangents use a stable local basis, transformed like an ordinary tangent.
        // Relative tolerance catches cancellation residue from nearly parallel finite vectors.
        // Non-finite transforms remain invalid; this fallback only handles finite zero-length tangents.
        vec3 axis = abs(normal.z) < 0.9 ? vec3(0, 0, 1) : vec3(0, 1, 0);
        t = mul(linear_matrix, cross(axis, normal));
        t -= n * dot(n, t);
    }
    t = normalize(t);
    float tangent_orientation = (tangent.w == 0 ? 1 : tangent.w) * handedness;
    vWorldTangent = vec4(t, tangent_orientation);
    vWorldBitangent = cross(n, t) * tangent_orientation;
#endif
#endif
    vMaterial = uvec2(property.material_buffer, property.material_record);
#endif
}

struct LuxInput
{
    [[vk::location(0)]] vec3 position : TEXCOORD0;
#if !defined(DEPTH_ONLY)
#if !defined(VERTEX_NO_UV)
    [[vk::location(1)]] vec2 uv : TEXCOORD1;
#endif
#endif
#if !defined(DEPTH_ONLY)
#if defined(VERTEX_NORMALS)
    [[vk::location(2)]] vec3 normal : TEXCOORD2;
#endif
#endif
#if !defined(DEPTH_ONLY)
#if defined(VERTEX_TANGENTS)
    [[vk::location(3)]] vec4 tangent : TEXCOORD3;
#endif
#endif
    uint vertexID : SV_VertexID;
    uint instanceID : SV_InstanceID;
#ifndef LUX_TARGET_DXIL
    uint baseInstance : SV_StartInstanceLocation;
#endif
};

struct LuxOutput
{
#if MESH_VARYINGS & 1
    [[vk::location(0)]] vec3 vWorldPos : TEXCOORD0;
#endif
#if MESH_VARYINGS & 2
    [[vk::location(1)]] vec3 vWorldNormal : TEXCOORD1;
#endif
#if MESH_VARYINGS & 4
    [[vk::location(3)]] vec2 vUV : TEXCOORD3;
#endif
#if MESH_VARYINGS & 8
    [[vk::location(4)]] nointerpolation uvec2 vMaterial : TEXCOORD4;
#endif
#if MESH_VARYINGS & 16
    [[vk::location(5)]] vec4 vWorldTangent : TEXCOORD5;
#endif
#if MESH_VARYINGS & 32
    [[vk::location(6)]] vec3 vWorldBitangent : TEXCOORD6;
#endif
    float4 position : SV_Position;
};
[shader("vertex")] LuxOutput main(LuxInput input)
{
    position = input.position;
#if !defined(DEPTH_ONLY)
#if !defined(VERTEX_NO_UV)
    uv = input.uv;
#endif
#endif
#if !defined(DEPTH_ONLY)
#if defined(VERTEX_NORMALS)
    normal = input.normal;
#endif
#endif
#if !defined(DEPTH_ONLY)
#if defined(VERTEX_TANGENTS)
    tangent = input.tangent;
#endif
#endif
    gl_VertexIndex = input.vertexID;
    gl_InstanceIndex = input.instanceID;
#ifndef LUX_TARGET_DXIL
    gl_InstanceIndex += input.baseInstance;
#endif
    luxMain();
    LuxOutput output;
#if MESH_VARYINGS & 1
    output.vWorldPos = vWorldPos;
#endif
#if MESH_VARYINGS & 2
    output.vWorldNormal = vWorldNormal;
#endif
#if MESH_VARYINGS & 4
    output.vUV = vUV;
#endif
#if MESH_VARYINGS & 8
    output.vMaterial = vMaterial;
#endif
#if MESH_VARYINGS & 16
    output.vWorldTangent = vWorldTangent;
#endif
#if MESH_VARYINGS & 32
    output.vWorldBitangent = vWorldBitangent;
#endif
    output.position = gl_Position;
#ifdef LUX_TARGET_DXIL
    output.position.y = -output.position.y;
#endif
    return output;
}
