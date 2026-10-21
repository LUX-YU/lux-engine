#version 450

void main()
{
    // Legacy full-screen triangle: no vertex Buffer, interpolants or View lookup.
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
