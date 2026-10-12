shader_type canvas_item;
render_mode unshaded;

// UV belongs to the physical screen, not to the high-resolution texel grid.
// The mask always stays at native 256x192 resolution, without filtering.
uniform sampler2D three_d_mask : filter_nearest, repeat_disable;

void fragment() {
	if (texture(three_d_mask, UV).r == 0.0) {
		discard;
	}
	COLOR = texture(TEXTURE, UV);
}
