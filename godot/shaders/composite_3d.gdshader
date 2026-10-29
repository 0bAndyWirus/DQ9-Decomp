shader_type canvas_item;
render_mode unshaded;

// The center retains the native mask; side regions contain only 3D.
uniform sampler2D three_d_mask : filter_nearest, repeat_disable;
uniform float width_ratio = 1.0;

void fragment() {
	float screen_x = (UV.x - 0.5) * width_ratio + 0.5;
	if (screen_x >= 0.0 && screen_x < 1.0 &&
			texture(three_d_mask, vec2(screen_x, UV.y)).r == 0.0) {
		discard;
	}
	COLOR = texture(TEXTURE, UV);
}
