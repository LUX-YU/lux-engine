#version 450
layout(location=0) in vec4 position;
layout(location=0) out vec4 vertex_color;
struct Transform { mat4 model; };
struct Word { uint value; };
layout(set=3,binding=7,std430) readonly buffer Transforms { Transform records[]; } transforms;
layout(set=2,binding=6,std430) readonly buffer Members { Word words[]; } members;
layout(push_constant) uniform Camera { mat4 view; mat4 projection; } camera;
void main()
{
    uint slot = members.words[5 + gl_InstanceIndex].value;
    gl_Position = camera.projection * camera.view * transforms.records[slot].model * position;
    vertex_color = slot == 0 ? vec4(1,0,0,1) : vec4(0,1,0,1);
}
