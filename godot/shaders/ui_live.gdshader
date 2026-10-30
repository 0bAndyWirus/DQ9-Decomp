shader_type canvas_item;
uniform sampler2D gpu_texture : filter_linear, repeat_disable;
uniform sampler2D gpu_mask : filter_nearest, repeat_disable;
uniform bool use_gpu = false;

vec4 live_sample(vec4 software, vec2 uv) {
	if (use_gpu && texture(gpu_mask, uv).r > 0.0) {
		return texture(gpu_texture, uv);
	}
	return software;
}

void fragment() {
	vec2 px = TEXTURE_PIXEL_SIZE * 1.5;
	vec4 image = live_sample(texture(TEXTURE, UV), UV) * 0.4;
	image += live_sample(texture(TEXTURE, UV + vec2(px.x, 0.0)), UV + vec2(px.x, 0.0)) * 0.15;
	image += live_sample(texture(TEXTURE, UV - vec2(px.x, 0.0)), UV - vec2(px.x, 0.0)) * 0.15;
	image += live_sample(texture(TEXTURE, UV + vec2(0.0, px.y)), UV + vec2(0.0, px.y)) * 0.15;
	image += live_sample(texture(TEXTURE, UV - vec2(0.0, px.y)), UV - vec2(0.0, px.y)) * 0.15;
	float left = mix(0.25, 0.65, smoothstep(0.2, 0.75, UV.x));
	float vignette = mix(0.55, 1.0, smoothstep(0.9, 0.15, length(UV - 0.5)));
	COLOR = vec4(image.rgb * left * vignette, image.a);
}
