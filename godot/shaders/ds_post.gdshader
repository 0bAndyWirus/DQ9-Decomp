shader_type spatial;
render_mode unshaded, cull_disabled, depth_test_disabled, depth_draw_never, blend_mix, shadows_disabled, ambient_light_disabled, fog_disabled;

#include "res://shaders/ds_color_transfer.gdshaderinc"
// This pass must live in the metadata viewport: Godot cannot sample another
// viewport's hardware depth. Its opaque ID pass supplies both screen and depth.
uniform sampler2D id_texture : hint_screen_texture, filter_nearest, repeat_disable;
uniform sampler2D depth_texture : hint_depth_texture, filter_nearest, repeat_disable;
uniform sampler2D color_texture : filter_nearest, repeat_disable;
uniform sampler2D fog_table : filter_nearest, repeat_disable;
uniform sampler2D edge_color_table : filter_nearest, repeat_disable;
uniform int disp3dcnt = 0;
uniform int fog_color = 0;
uniform int fog_offset = 0;
uniform int clear_id = 0;
uniform int clear_depth = 32767;
uniform int clear_color = 0;
// Display-only opaque-mask output, never a general straight-alpha 3D target.
// Partial-alpha pixels remain software; replacing metadata must not blend it.
uniform bool opaque_mask_output = false;
uniform bool debug_depth = false;

void vertex() {
	POSITION = vec4(VERTEX.xy, 1.0, 1.0);
}

int ds_depth(float reverse_depth) {
	return int(floor(clamp(1.0 - reverse_depth, 0.0, 1.0) * 4194303.0)) * 4;
}

int ds_clear_depth() {
	return (clear_depth & 32767) * 512 + 511;
}

ivec3 metadata(vec4 value) {
	return value.a == 0.0 ? ivec3(clear_id, (clear_color & 32768) != 0 ? 1 : 0, (clear_color >> 16) & 31) :
		ivec3(floor((value.rgb * 255.0 - 64.0) / vec3(3.0, 48.0, 6.0) + 0.5));
}

int density_at(int index) {
	return int(floor(texture(fog_table, vec2((float(index) + 0.5) / 32.0, 0.5)).r * 255.0 + 0.5)) & 127;
}

int fog_weight(int depth) {
	int step_size = 1024 >> ((disp3dcnt >> 8) & 15);
	int depth_value = depth >> 9;
	int offset = fog_offset & 32767;
	int density;
	if (step_size == 0) {
		density = density_at(depth_value <= offset ? 0 : 31);
	} else {
		int relative = max(depth_value - offset, 0);
		if (relative <= step_size) { density = density_at(0); }
		else if (relative >= step_size * 32) { density = density_at(31); }
		else {
			int segment = relative / step_size;
			int fraction = relative - segment * step_size;
			density = (density_at(segment - 1) * (step_size - fraction) + density_at(segment) * fraction) / step_size;
		}
	}
	return density == 127 ? 128 : density;
}

int expand5(int value) { return value == 0 ? 0 : value * 2 + 1; }

void fragment() {
	vec2 uv = SCREEN_UV;
	// Screen/depth samplers must be read in fragment(), not helper functions:
	// Godot 4.5 Compatibility otherwise emits helpers before their declarations.
	vec4 raw_metadata = texture(id_texture, uv);
	int depth = raw_metadata.a == 0.0 ? ds_clear_depth() : ds_depth(texture(depth_texture, uv).r);
	if (debug_depth) {
		ALBEDO = vec3(float(ds_depth(texture(depth_texture, uv).r)) / 16777212.0);
		ALPHA = 1.0;
	} else {
		vec4 source = texture(color_texture, uv);
		// Empty clear pixels can acquire an outline from a nearer neighbour.
		// Discard only after edges/fog have had a chance to write their alpha.
		ivec3 pixel = metadata(raw_metadata);
		int clear_alpha = (clear_color >> 16) & 31;
		bool rear_plane = raw_metadata.a == 0.0 || (clear_alpha != 31 && pixel.z == clear_alpha);
		// Clear pixels need their original straight RGB6 and alpha, not the
		// quantized premultiplied color or GL blend against earlier geometry.
		ivec3 rgb = rear_plane ?
			ivec3(expand5(clear_color & 31), expand5((clear_color >> 5) & 31), expand5((clear_color >> 10) & 31)) :
			ivec3(floor(source.rgb * 63.0 + 0.5));
		int alpha5 = rear_plane ? clear_alpha : int(floor(source.a * 31.0 + 0.5));
		if ((disp3dcnt & 32) != 0 && (pixel.y & 2) == 0) {
			vec2 texel = 1.0 / VIEWPORT_SIZE;
			vec2 offsets[4] = vec2[](vec2(texel.x, 0.0), vec2(0.0, texel.y), vec2(-texel.x, 0.0), vec2(0.0, -texel.y));
			bool marked = false;
			int edge_id = pixel.x;
			for (int i = 0; i < 4; ++i) {
				vec2 neighbor_uv = uv + offsets[i];
				if (any(lessThan(neighbor_uv, vec2(0.0))) || any(greaterThanEqual(neighbor_uv, vec2(1.0)))) {
					marked = pixel.x != clear_id && depth < ds_clear_depth();
				} else {
					vec4 raw_neighbor = texture(id_texture, neighbor_uv);
					ivec3 neighbor = metadata(raw_neighbor);
					int neighbor_depth = raw_neighbor.a == 0.0 ? ds_clear_depth() : ds_depth(texture(depth_texture, neighbor_uv).r);
					marked = pixel.x != neighbor.x && depth >= neighbor_depth;
					if (marked) { edge_id = neighbor.x; }
				}
				if (marked) { break; }
			}
			if (marked) {
				ivec3 edge_rgb = ivec3(floor(texture(edge_color_table, vec2((float(edge_id / 8) + 0.5) / 8.0, 0.5)).rgb * 63.0 + 0.5));
				int edge_alpha = (disp3dcnt & 16) != 0 ? 16 : 31;
				rgb = edge_alpha == 31 || alpha5 == 0 ? edge_rgb : (edge_rgb * (edge_alpha + 1) + rgb * (31 - edge_alpha)) / 32;
				alpha5 = max(alpha5, edge_alpha);
			}
		}
		if ((disp3dcnt & 128) != 0 && (pixel.y & 1) != 0) {
			int weight = fog_weight(depth);
			if ((disp3dcnt & 64) == 0) {
				ivec3 fog_rgb = ivec3(expand5(fog_color & 31), expand5((fog_color >> 5) & 31), expand5((fog_color >> 10) & 31));
				rgb = (fog_rgb * weight + rgb * (128 - weight)) / 128;
			}
			alpha5 = min(31, (((fog_color >> 16) & 31) * weight + alpha5 * (128 - weight)) / 128);
		}
		if (alpha5 == 0) { discard; }
		ALBEDO = ds_rgb6_output(vec3(rgb));
		// Opaque geometry has alpha31; native mask=0 excludes partial clear,
		// antialiased edges and fog-created partial pixels. Replace the ID
		// framebuffer there rather than contaminating covered output with it.
		ALPHA = opaque_mask_output ? 1.0 : float(alpha5) / 31.0;
	}
}
