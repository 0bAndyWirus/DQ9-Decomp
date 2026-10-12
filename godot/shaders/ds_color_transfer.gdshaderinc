// Godot 4.5 Compatibility applies a cubic sRGB->linear approximation followed
// by the power-law inverse even for unshaded ALBEDO. A 256-entry inverse table
// compensates that documented engine transform, including its dark-range loss.
// This is an RF data texture, not source_color. It is generated once on the CPU.
uniform sampler2D color_transfer : filter_nearest, repeat_disable;
// Only the final color pass truncates RGB6 to the LCD compositor's RGB555.
uniform bool display_rgb555 = false;

vec3 ds_rgb8_output(ivec3 bytes) {
	return vec3(texture(color_transfer, vec2((float(bytes.r) + 0.5) / 256.0, 0.5)).r,
		texture(color_transfer, vec2((float(bytes.g) + 0.5) / 256.0, 0.5)).r,
		texture(color_transfer, vec2((float(bytes.b) + 0.5) / 256.0, 0.5)).r);
}

vec3 ds_rgb6_output(vec3 rgb6) {
	ivec3 value = ivec3(rgb6);
	if (display_rgb555) {
		value >>= 1;
		return ds_rgb8_output((value << 3) | (value >> 2));
	}
	return ds_rgb8_output((value << 2) | (value >> 4));
}
