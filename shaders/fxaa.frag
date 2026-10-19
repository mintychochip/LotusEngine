#version 450

// FXAA 3.11 console post pass (Timothy Lottes' console preset, the short
// variant: 5 cross taps + 4 diagonal taps + 4 along the dominant edge).
//
// Luma is the Rec.601 dot, computed per tap -- deliberately NOT the
// green-as-luma shortcut, so saturated red/blue edges get the same treatment
// as green ones. Runs on the LDR sRGB image the scene pass already encoded.
// Single colour target; the resolve pass binds it onto the real destination
// (swapchain, viewport texture, or offscreen target).

layout(location = 0) in vec2 v_uv;

layout(set = 2, binding = 0) uniform sampler2D source;

layout(set = 3, binding = 0) uniform FxaaUniforms {
    vec4 texel; // x: 1/width; y: 1/height; zw reserved
} u;

layout(location = 0) out vec4 out_color;

// Console preset constants (FXAA_SPAN_MAX 8, REDUCE_MUL 1/8, REDUCE_MIN
// 1/128, edge threshold 1/8 with a 0.0312 floor).
const float SPAN_MAX        = 8.0;
const float REDUCE_MUL      = 1.0 / 8.0;
const float REDUCE_MIN      = 1.0 / 128.0;
const float EDGE_THRESHOLD  = 1.0 / 8.0;
const float EDGE_THRESHOLD_MIN = 0.0312;

float fxaa_luma(vec3 c)
{
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void main()
{
    vec2 texel = u.texel.xy;

    vec3 rgbM = texture(source, v_uv).rgb;
    float lumaM = fxaa_luma(rgbM);
    float lumaN = fxaa_luma(texture(source, v_uv + vec2(0.0, -texel.y)).rgb);
    float lumaS = fxaa_luma(texture(source, v_uv + vec2(0.0, texel.y)).rgb);
    float lumaE = fxaa_luma(texture(source, v_uv + vec2(texel.x, 0.0)).rgb);
    float lumaW = fxaa_luma(texture(source, v_uv + vec2(-texel.x, 0.0)).rgb);

    float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float range   = lumaMax - lumaMin;
    if (range < max(EDGE_THRESHOLD_MIN, lumaMax * EDGE_THRESHOLD)) {
        out_color = vec4(rgbM, 1.0);
        return;
    }

    float lumaNW =
            fxaa_luma(texture(source, v_uv + vec2(-texel.x, -texel.y)).rgb);
    float lumaNE =
            fxaa_luma(texture(source, v_uv + vec2(texel.x, -texel.y)).rgb);
    float lumaSW =
            fxaa_luma(texture(source, v_uv + vec2(-texel.x, texel.y)).rgb);
    float lumaSE =
            fxaa_luma(texture(source, v_uv + vec2(texel.x, texel.y)).rgb);

    vec2 dir = vec2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)),
                    ((lumaNW + lumaSW) - (lumaNE + lumaSE)));
    float dirReduce =
            max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * REDUCE_MUL,
                REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = min(vec2(SPAN_MAX), max(vec2(-SPAN_MAX), dir * rcpDirMin)) * texel;

    vec3 rgbA = 0.5 *
            (texture(source, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb +
             texture(source, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 +
                0.25 *
                (texture(source, v_uv + dir * -0.5).rgb +
                 texture(source, v_uv + dir * 0.5).rgb);
    float lumaB = fxaa_luma(rgbB);
    if (lumaB < lumaMin || lumaB > lumaMax) {
        out_color = vec4(rgbA, 1.0);
    } else {
        out_color = vec4(rgbB, 1.0);
    }
}
