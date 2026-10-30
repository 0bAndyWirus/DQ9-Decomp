shader_type spatial;
// One partial fragment over an opaque destination. The producer excludes
// overlapping histories, translucent depth writes and fog-flag changes.
// ALPHA=1 places this after the opaque screen snapshot without GL blending.
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never, shadows_disabled, ambient_light_disabled, fog_disabled;
#include "res://shaders/ds_poly_common.gdshaderinc"
uniform sampler2D opaque_color : hint_screen_texture, filter_nearest, repeat_disable;

void vertex() {
	POSITION = ds_position(VERTEX, UV2.x, int(UV2.y), CUSTOM1.rgb);
	ds_clip_w = UV2.x;
	ds_vertex_color = span_mesh ? CUSTOM0.rgb : COLOR.rgb;
}

void fragment() {
	DEPTH = ds_fragment_depth(FRAGCOORD.z);
	vec4 shade = ds_shade(UV, ds_vertex_color);
	if (shade.a == 0.0 || shade.a == 31.0 ||
		(alpha_test_enabled && shade.a < float(alpha_test_ref))) { discard; }
	vec3 destination = floor(texture(opaque_color, SCREEN_UV).rgb * 63.0 + 0.5);
	vec3 blended = floor((shade.rgb * (shade.a + 1.0) + destination * (31.0 - shade.a)) / 32.0);
	ALBEDO = ds_rgb6_output(blended);
	ALPHA = 1.0;
}
