shader_type spatial;
// Prepend DS_OPAQUE, DS_NO_DEPTH_WRITE or DS_ADDITIVE for material variants.
#ifdef DS_ADDITIVE
render_mode unshaded, cull_disabled, blend_add, depth_draw_never, shadows_disabled, ambient_light_disabled, fog_disabled;
#else
#ifdef DS_NO_DEPTH_WRITE
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never, shadows_disabled, ambient_light_disabled, fog_disabled;
#else
render_mode unshaded, cull_disabled, blend_mix, depth_draw_always, shadows_disabled, ambient_light_disabled, fog_disabled;
#endif
#endif

#include "res://shaders/ds_poly_common.gdshaderinc"

void vertex() {
	POSITION = ds_position(VERTEX, UV2.x, int(UV2.y), CUSTOM1.rgb);
	ds_clip_w = UV2.x;
	ds_vertex_color = span_mesh ? CUSTOM0.rgb : COLOR.rgb;
}

void fragment() {
	DEPTH = ds_fragment_depth(FRAGCOORD.z);
	vec4 shade = ds_shade(UV, ds_vertex_color);
	// Alpha zero is wireframe in the DS. The caller must skip/route those polygons.
	if (shade.a == 0.0 || (alpha_test_enabled && shade.a < float(alpha_test_ref))) { discard; }
	float native_x = (SCREEN_UV.x - 0.5) * side_native_width_ratio + 0.5;
	// DS geometry starts half a native pixel in, reduced by span overlap.
	// Neutralize only the potentially exposed clear strip in wide mode.
	const float phase = 0.5 - 1.0 / 32.0;
	bool side_clear = clear_plane && side_native_width_ratio > 1.0 &&
		(native_x < phase * side_native_width_ratio / 256.0 || native_x >= 1.0 ||
		 SCREEN_UV.y < phase / 192.0);
	ALBEDO = side_clear ? vec3(0.0) : ds_rgb6_output(shade.rgb);
	// The partial-fragment pass blends against this opaque-only snapshot.
	// Decal texel alpha does not change fragment alpha.
#ifdef DS_OPAQUE
	if (span_mesh && shade.a < 31.0) { discard; }
#endif
#ifndef DS_OPAQUE
	ALPHA = shade.a / 31.0;
#endif
}
