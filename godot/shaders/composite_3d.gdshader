shader_type canvas_item;
render_mode unshaded;

// The center retains the native mask; side regions contain only 3D.
uniform sampler2D three_d_mask : filter_nearest, repeat_disable;
uniform float width_ratio = 1.0;
uniform bool side_fallback_enabled = false;
uniform sampler2D side_software_texture : filter_nearest, repeat_disable;
uniform sampler2D side_eligibility_texture : filter_nearest, repeat_disable;

void fragment() {
	float screen_x = (UV.x - 0.5) * width_ratio + 0.5;
	if (screen_x >= 0.0 && screen_x < 1.0 &&
			texture(three_d_mask, vec2(screen_x, UV.y)).r == 0.0) {
		discard;
	}
	vec4 gpu = texture(TEXTURE, UV);
	if (side_fallback_enabled && (screen_x < 0.0 || screen_x >= 1.0) &&
			texture(side_eligibility_texture, UV).r < 0.5) {
		vec4 software = texture(side_software_texture, UV);
		if (software.a > 0.5) gpu = software;
	}
	COLOR = gpu;
}
