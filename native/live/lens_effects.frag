#version 450

layout(set = 0, binding = 0) uniform sampler2D source_image;
layout(push_constant) uniform Parameters {
    vec2 inverse_extent;
    float distortion;
    float vignette;
    float chromatic_pixels;
    float encode_output;
} parameters;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

vec3 encode_srgb(vec3 value) {
    value = clamp(value, 0, 1);
    return mix(1.055 * pow(value, vec3(1.0 / 2.4)) - .055,
               12.92 * value, lessThanEqual(value, vec3(.0031308)));
}

// Explicit bilinear interpolation avoids implementation-specific fixed-point
// sampler weights. Offline frame processing uses the same pixel-center mapping.
vec3 sample_linear(vec2 point) {
    ivec2 size = textureSize(source_image, 0);
    vec2 position = point * vec2(size) - .5;
    ivec2 base = ivec2(floor(position));
    vec2 fraction = fract(position);
    ivec2 limit = size - 1;
    vec3 a = texelFetch(source_image, clamp(base, ivec2(0), limit), 0).rgb;
    vec3 b = texelFetch(source_image, clamp(base + ivec2(1, 0), ivec2(0), limit), 0).rgb;
    vec3 c = texelFetch(source_image, clamp(base + ivec2(0, 1), ivec2(0), limit), 0).rgb;
    vec3 d = texelFetch(source_image, clamp(base + ivec2(1, 1), ivec2(0), limit), 0).rgb;
    return mix(mix(a, b, fraction.x), mix(c, d, fraction.x), fraction.y);
}

void main() {
    vec2 centered = uv * 2 - 1;
    float aspect = parameters.inverse_extent.y / parameters.inverse_extent.x;
    vec2 radial = centered * vec2(aspect, 1);
    float radius_squared = dot(radial, radial) / (aspect * aspect + 1);
    // The bounded coefficient keeps dr'/dr positive at every radius. Fitting
    // positive distortion to the corners avoids black borders or reflected UVs.
    float coefficient = .4 * parameters.distortion;
    float scale = (1 + coefficient * radius_squared) / (1 + max(coefficient, 0));
    vec2 sample_uv = centered * scale * .5 + .5;
    vec2 direction = radial / max(length(radial), .000001);
    vec2 separation = direction * parameters.inverse_extent * parameters.chromatic_pixels * radius_squared;
    vec3 linear_rgb = sample_linear(sample_uv);
    if (parameters.chromatic_pixels > 0) {
        linear_rgb.r = sample_linear(sample_uv + separation).r;
        linear_rgb.b = sample_linear(sample_uv - separation).b;
    }
    linear_rgb *= 1 - parameters.vignette * smoothstep(.2, 1, radius_squared);
    color = vec4(parameters.encode_output > .5 ? encode_srgb(linear_rgb) : linear_rgb, 1);
}
