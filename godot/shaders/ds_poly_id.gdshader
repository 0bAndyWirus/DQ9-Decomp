shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_always, shadows_disabled, ambient_light_disabled, fog_disabled;
#include "res://shaders/ds_poly_common.gdshaderinc"
uniform int polygon_id : hint_range(0, 63) = 0;
uniform bool polygon_fog = false;
uniform bool translucent_polygon = false;
uniform bool vertex_metadata = false;
uniform bool discard_partial = false;
uniform bool discard_surface = false;
uniform bool coverage_only = false;
varying flat int packed_metadata;

void vertex() {
	POSITION = ds_position(VERTEX, UV2.x, int(UV2.y), CUSTOM1.rgb);
	ds_clip_w = UV2.x;
	ds_vertex_color = span_mesh ? CUSTOM0.rgb : COLOR.rgb;
	packed_metadata = int(UV2.y);
}

void fragment() {
	DEPTH = ds_fragment_depth(FRAGCOORD.z);
	vec4 shade = ds_shade(UV, ds_vertex_color);
	if (shade.a == 0.0 || (alpha_test_enabled && shade.a < float(alpha_test_ref))) { discard; }
	if (discard_surface || (discard_partial && shade.a < 31.0)) { discard; }
	if (coverage_only) {
		// Clear writes depth but not coverage; later/nearer geometry writes white.
		ALBEDO = clear_plane ? vec3(0.0) : vec3(1.0);
	} else {
		// Compatibility's approximate sRGB round trip loses small data bytes.
		// R=64+id*3, G=64+flags*48, B=64+alpha5*6.
		int id = vertex_metadata ? (packed_metadata & 63) : polygon_id;
		int flags = vertex_metadata ? ((packed_metadata >> 6) & 3) :
			((polygon_fog ? 1 : 0) + (translucent_polygon ? 2 : 0));
		float alpha5 = vertex_metadata ? float((packed_metadata >> 8) & 31) : shade.a;
		// Reserve green=255 for the synthetic rear plane; real flags end at 208.
		ALBEDO = ds_rgb8_output(ivec3(64 + id * 3, clear_plane ? 255 : 64 + flags * 48, 64 + int(alpha5) * 6));
	}
}
