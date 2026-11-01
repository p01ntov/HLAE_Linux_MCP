#version 450

layout(location = 0) out vec2 uv;
void main() {
    const vec2 triangle[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
    vec2 position = triangle[gl_VertexIndex];
    gl_Position = vec4(position, 0, 1);
    uv = position * .5 + .5;
}
