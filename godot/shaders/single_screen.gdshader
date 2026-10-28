shader_type canvas_item;

uniform vec4 source_rect = vec4(0.0, 0.0, 1.0, 1.0);
uniform bool linear_filter = false;
uniform bool use_color_key = false;
uniform vec4 color_key = vec4(0.0);
uniform bool use_gpu = false;
uniform sampler2D gpu_texture : filter_nearest, repeat_disable;
uniform sampler2D gpu_mask : filter_nearest, repeat_disable;
varying vec4 vertex_color;

void vertex() {
	vertex_color = COLOR;
}

void fragment() {
	vec2 screen_uv = source_rect.xy + UV * source_rect.zw;
	vec2 sample_uv = screen_uv;
	if (linear_filter) {
		vec2 dimensions = vec2(textureSize(TEXTURE, 0));
		// Include every texel intersected by the crop, even for subpixel crops.
		vec2 crop_min = (floor(source_rect.xy * dimensions) + 0.5) / dimensions;
		vec2 crop_max = (ceil((source_rect.xy + source_rect.zw) * dimensions) - 0.5) / dimensions;
		sample_uv = clamp(screen_uv, crop_min, crop_max);
	}
	vec4 pixel = texture(TEXTURE, sample_uv);
	ivec2 key_size = textureSize(TEXTURE, 0);
	ivec2 key_coord = clamp(ivec2(screen_uv * vec2(key_size)), ivec2(0), key_size - ivec2(1));
	vec4 key_pixel = texelFetch(TEXTURE, key_coord, 0);
	if (use_gpu && texture(gpu_mask, screen_uv).r != 0.0) {
		vec4 gpu_pixel = texture(gpu_texture, screen_uv);
		float alpha = gpu_pixel.a + pixel.a * (1.0 - gpu_pixel.a);
		float key_alpha = gpu_pixel.a + key_pixel.a * (1.0 - gpu_pixel.a);
		pixel.rgb = alpha > 0.0 ? (gpu_pixel.rgb * gpu_pixel.a + pixel.rgb * pixel.a * (1.0 - gpu_pixel.a)) / alpha : vec3(0.0);
		key_pixel.rgb = key_alpha > 0.0 ? (gpu_pixel.rgb * gpu_pixel.a + key_pixel.rgb * key_pixel.a * (1.0 - gpu_pixel.a)) / key_alpha : vec3(0.0);
		pixel.a = alpha;
		key_pixel.a = key_alpha;
	}
	if (use_color_key && all(lessThan(abs(key_pixel.rgb - color_key.rgb), vec3(0.5 / 255.0)))) {
		pixel.a = 0.0;
	}
	COLOR = vertex_color * pixel;
}
