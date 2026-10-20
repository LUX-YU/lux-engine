#ifndef LUX_TEXTURE_SAMPLING_GLSL
#define LUX_TEXTURE_SAMPLING_GLSL

// Fragment derivatives must be evaluated before the per-material default branch.
vec4 luxSampleTexture(TextureRefGPU reference, vec2 uv)
{
    vec2 uv_dx = dFdx(uv);
    vec2 uv_dy = dFdy(uv);
    if ((reference.flags & LUX_TEXTURE_DEFAULT_WHITE) != 0u)
    {
        return vec4(1.0);
    }
    if (reference.representation_index == 0u)
    {
        return textureGrad(
            sampler2D(uTex[nonuniformEXT(reference.resource_index)],
                uTexSamplers[nonuniformEXT(reference.sampler_index)]),
            uv, uv_dx, uv_dy);
    }
    return vec4(1.0, 0.0, 1.0, 1.0);
}

#endif // LUX_TEXTURE_SAMPLING_GLSL
