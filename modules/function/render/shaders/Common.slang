#ifndef LUX_SHADER_COMMON
#define LUX_SHADER_COMMON
#define vec2 float2
#define vec3 float3
#define vec4 float4
#define ivec2 int2
#define ivec3 int3
#define uvec2 uint2
#define uvec3 uint3
#define uvec4 uint4
#define mat3 float3x3
#define mat4 float4x4

float mix(float a, float b, float t)
{
    return lerp(a, b, t);
}

float3 mix(float3 a, float3 b, float t)
{
    return lerp(a, b, t);
}

float3 mix(float3 a, float3 b, bool3 t)
{
    return lerp(a, b, float3(t));
}

#define uintBitsToFloat asfloat
#define floatBitsToUint asuint
#define fract frac
#define inversesqrt rsqrt
#define nonuniformEXT NonUniformResourceIndex
#define barrier GroupMemoryBarrierWithGroupSync
#define greaterThanEqual(a, b) ((a) >= (b))
#define lessThanEqual(a, b) ((a) <= (b))
#define greaterThan(a, b) ((a) > (b))
#define lessThan(a, b) ((a) < (b))
#define equal(a, b) ((a) == (b))
static uint3 gl_GlobalInvocationID, gl_WorkGroupID;
static uint gl_LocalInvocationIndex, gl_VertexIndex, gl_InstanceIndex;
static float4 gl_Position, gl_FragCoord;

uint luxAtomicAdd(inout uint destination, uint value)
{
    uint previous;
    InterlockedAdd(destination, value, previous);
    return previous;
}

uint luxAtomicOr(inout uint destination, uint value)
{
    uint previous;
    InterlockedOr(destination, value, previous);
    return previous;
}

#define atomicAdd luxAtomicAdd
#define atomicOr luxAtomicOr

uint luxLength<T>(StructuredBuffer<T> buffer)
{
    uint count, stride;
    buffer.GetDimensions(count, stride);
    return count;
}

uint luxLength<T>(RWStructuredBuffer<T> buffer)
{
    uint count, stride;
    buffer.GetDimensions(count, stride);
    return count;
}

int2 textureSize(Texture2D<float4> image, int mip)
{
    uint width, height, levels;
    image.GetDimensions(mip, width, height, levels);
    return int2(width, height);
}

int2 imageSize(RWTexture2D<float> image)
{
    uint width, height;
    image.GetDimensions(width, height);
    return int2(width, height);
}

float4 texelFetch(Texture2D<float4> image, int2 pixel, int mip)
{
    return image.Load(int3(pixel, mip));
}

int3 textureSize(Texture2DArray<float> image, int mip)
{
    uint w, h, l, n;
    image.GetDimensions(mip, w, h, l, n);
    return int3(w, h, l);
}

// Row operations compute the same inverse matrix without relying on a GLSL-only intrinsic.
float4x4 inverse(float4x4 value)
{
    float4x4 result = float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    for (uint column = 0; column < 4; ++column)
    {
        uint pivot = column;
        for (uint row = column + 1; row < 4; ++row)
        {
            if (abs(value[row][column]) > abs(value[pivot][column]))
            {
                pivot = row;
            }
        }
        if (value[pivot][column] == 0)
        {
            return float4x4(asfloat(0x7fc00000u));
        }
        float4 swap_row = value[column];
        value[column] = value[pivot];
        value[pivot] = swap_row;
        swap_row = result[column];
        result[column] = result[pivot];
        result[pivot] = swap_row;
        float divisor = value[column][column];
        value[column] /= divisor;
        result[column] /= divisor;
        for (uint row = 0; row < 4; ++row)
        {
            if (row == column)
            {
                continue;
            }
            float factor = value[row][column];
            value[row] -= value[column] * factor;
            result[row] -= result[column] * factor;
        }
    }
    return result;
}
#endif
