#ifndef MATERIAL_TYPES_SLANG
#define MATERIAL_TYPES_SLANG

// The compiler emits each material record from its cooked parameter layout.
// Texture sampling shares only this reference representation.
struct TextureRefGPU
{
    uint representation_index;
    uint resource_index;
    uint sampler_index;
    uint flags;
};

#endif
