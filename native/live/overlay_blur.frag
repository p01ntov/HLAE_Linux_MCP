#version 450

layout(set = 0, binding = 0) uniform sampler2D source_image;
layout(push_constant) uniform Direction { vec2 texel_step; float encoded_dim_alpha; } direction;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

vec3 encode_srgb(vec3 linear_rgb) {
    vec3 value = clamp(linear_rgb, 0.0, 1.0);
    return mix(1.055 * pow(value, vec3(1.0 / 2.4)) - 0.055,
               12.92 * value, lessThanEqual(value, vec3(0.0031308)));
}

vec3 decode_srgb(vec3 encoded_rgb) {
    return mix(pow((encoded_rgb + 0.055) / 1.055, vec3(2.4)),
               encoded_rgb / 12.92, lessThanEqual(encoded_rgb, vec3(0.04045)));
}

// Normalized Gaussian, sigma=2 pixels, radius=6. Each pair of adjacent taps
// uses one bilinear sample: the resulting kernel equals the 13 discrete taps.
// The source SRGB image is decoded by its sampler. Intermediate RGBA16F images
// and the returned ImGui texture retain linear RGB throughout both passes.
void main() {
    vec3 sum = texture(source_image, uv).rgb * 0.1996756274979211;
    const float weights[3] = float[3](0.29732251286339895, 0.09184834274140634, 0.01099133064623415);
    const float offsets[3] = float[3](1.4073334000459303, 3.294214972162989, 5.201813222260379);
    for (int tap = 0; tap < 3; ++tap) {
        vec2 delta = direction.texel_step * offsets[tap];
        sum += (texture(source_image, uv + delta).rgb + texture(source_image, uv - delta).rgb) * weights[tap];
    }
    // Panorama's CSS #000000a0 dim is authored in encoded sRGB. Reproduce that
    // brightness exactly, then return linear RGB for the SRGB ImGui attachment.
    // The horizontal pass receives alpha=0, so dimming happens only once.
    if (direction.encoded_dim_alpha > 0.0)
        sum = decode_srgb(encode_srgb(sum) * (1.0 - direction.encoded_dim_alpha));
    color = vec4(sum, 1.0);
}
