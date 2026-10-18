#ifndef MATERIAL_TYPES_GLSL
#define MATERIAL_TYPES_GLSL

// The compiler emits each material record from its cooked parameter layout.
// Texture sampling shares only this reference representation.
struct TextureRefGPU
{
    uint representation_index;
    uint resource_index;
    uint aux;
    uint flags;
};

#endif
