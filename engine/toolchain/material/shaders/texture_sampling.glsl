#ifndef LUX_TEXTURE_SAMPLING_GLSL
#define LUX_TEXTURE_SAMPLING_GLSL

// Index 0 is the production bindless representation. Additional providers add
// generated cases during shader assembly; test-only providers do not ship in
// this production source.
vec4 luxSampleTexture(TextureRefGPU reference, vec2 uv)
{
    if (reference.representation_index == 0u)
    {
        return texture(
            uTex[nonuniformEXT(reference.resource_index)],
            uv);
    }
    return vec4(1.0, 0.0, 1.0, 1.0);
}

#endif // LUX_TEXTURE_SAMPLING_GLSL
