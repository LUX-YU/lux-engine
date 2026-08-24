#include <lux/engine/function/render/features/BuiltinContentProvider.hpp>

#include <cassert>

int main()
{
    const auto content = lux::render::builtinShaderContent(
        lux::render::EBuiltinShader{}
    );
    assert(!content);
    assert(content.error() ==
        lux::render::EBuiltinContentError::BUILTIN_CONTENT_UNAVAILABLE);
}
