#include "res://shaders/ds_color_transfer.gdshaderinc"
// Inputs are data, not sRGB textures. Expanded RGB6/alpha5 are quantised here.
uniform sampler2D ds_texture : filter_nearest, repeat_disable;
uniform sampler2D toon_table : filter_nearest, repeat_disable;
uniform bool textured = false;
uniform int polygon_mode : hint_range(0, 2) = 0;
uniform bool highlight = false;
uniform int polygon_alpha : hint_range(0, 31) = 31;
uniform bool alpha_test_enabled = false;
uniform int alpha_test_ref : hint_range(0, 31) = 0;
uniform bvec2 texture_repeat = bvec2(false);
uniform bvec2 texture_flip = bvec2(false);
// Bottom-left origin and size, divided by (256,192); endpoints are inclusive.
uniform vec4 viewport_rect = vec4(0.0, 0.0, 1.0, 1.0);
// DqGpuScene strips are already full-screen clip coordinates. CUSTOM0 carries
// float RGB6/63, avoiding ArrayMesh's lossy eight-bit vertex COLOR packing.
uniform bool span_mesh = false;
// Half of the native 28.4 grid step closes snapped-edge T-junctions at hi-res.
// Scale1 sets zero: its sample positions/interpolation are never expanded.
uniform float span_margin = 0.0;
// The clear buffer fills the viewport, rather than passing through DS edges.
uniform bool clear_plane = false;
// Width of the wide target relative to the native 4:3 screen.
uniform float side_native_width_ratio = 1.0;
varying vec3 ds_vertex_color;

vec4 ds_position(vec3 clip_xyz, float clip_w, int vertex_metadata, vec3 depth_plane) {
	vec2 xy = span_mesh ? clip_xyz.xy : clip_xyz.xy * viewport_rect.zw +
		(2.0 * viewport_rect.xy + viewport_rect.zw - 1.0) * clip_w;
	float z = clip_xyz.z;
	// The DS edge walker truncates projected vertices to a 28.4 grid and
	// samples at integer coordinates, not the GL half-pixel centres. Keep the
	// same grid at every hi-res scale; the +0.5 DS-pixel nudge makes native
	// pixel centres evaluate the DS's integer sample locations.
	if (clip_w > 0.0 && !clear_plane) {
		vec2 screen = vec2((xy.x / clip_w + 1.0) * 128.0, (1.0 - xy.y / clip_w) * 96.0);
		if (!span_mesh) { screen = vec2(ivec2(screen * 16.0)) / 16.0; }
		screen += vec2(0.5);
		if (span_mesh) {
			int corner = (vertex_metadata >> 13) & 3;
			vec2 overlap = vec2(corner == 0 || corner == 3 ? -span_margin : span_margin,
				corner < 2 ? -span_margin : span_margin);
			screen += overlap;
			// Native span Z has per-row rounding/prestep discontinuities. In
			// hi-res use the polygon's one geometric plane, including overlap,
			// so adjacent strips cannot expose a coplanar surface behind it.
			if (span_margin > 0.0) { z = (depth_plane.z + dot(depth_plane.xy, overlap)) * clip_w; }
		}
		xy = vec2(screen.x / 128.0 - 1.0, 1.0 - screen.y / 96.0) * clip_w;
	}
	// Godot 4.3+ uses reverse Z. Compatibility NDC is [-1,1], DS near=-w.
#if CURRENT_RENDERER == RENDERER_COMPATIBILITY
	// SubViewport's Compatibility render target has an inverted clip-space Y.
	xy.y = -xy.y;
	return vec4(xy, -z, clip_w);
#else
	return vec4(xy, (clip_w - z) * 0.5, clip_w);
#endif
}

float ds_wrap(float coordinate, float extent, bool repeats, bool flips) {
	float value = floor(coordinate);
	if (!repeats) { return clamp(value, 0.0, extent - 1.0); }
	float period = extent * (flips ? 2.0 : 1.0);
	value = mod(value, period);
	return flips && value >= extent ? period - 1.0 - value : value;
}

float ds_expand5(float value) {
	return value == 0.0 ? 0.0 : value * 2.0 + 1.0;
}

vec4 ds_shade(vec2 uv, vec3 vertex_color) {
	vec3 vertex6 = clamp(floor(vertex_color * 63.0 + 0.5), vec3(0.0), vec3(63.0));
	vec4 texel = vec4(63.0, 63.0, 63.0, 31.0);
	if (textured) {
		vec2 dimensions = vec2(textureSize(ds_texture, 0));
		// Floor before reflecting: DS mirrored texels use period-1-integer_texel,
		// not a mirrored fractional UV (which is wrong on integer boundaries).
		vec2 texel_xy = vec2(ds_wrap(uv.x * dimensions.x, dimensions.x, texture_repeat.x, texture_flip.x),
			ds_wrap(uv.y * dimensions.y, dimensions.y, texture_repeat.y, texture_flip.y));
		vec4 sample_value = texture(ds_texture, (texel_xy + 0.5) / dimensions);
		texel = floor(sample_value * vec4(63.0, 63.0, 63.0, 31.0) + 0.5);
	}
	vec3 toon6 = vertex6;
	if (polygon_mode == 2) {
		float index = floor(vertex6.r * 0.5);
		toon6 = floor(texture(toon_table, vec2((index + 0.5) / 32.0, 0.5)).rgb * 63.0 + 0.5);
	}
	vec3 shade = polygon_mode == 2 && !highlight ? toon6 : vertex6;
	float alpha5 = float(polygon_alpha);
	if (polygon_mode == 0 || polygon_mode == 2) {
		vec3 modulation = highlight && polygon_mode == 2 ? vec3(vertex6.r) : shade;
		shade = floor(((texel.rgb + 1.0) * (modulation + 1.0) - 1.0) / 64.0);
		if (highlight && polygon_mode == 2) { shade = min(vec3(63.0), shade + toon6); }
	} else if (textured) {
		shade = texel.a == 0.0 ? vertex6 : (texel.a == 31.0 ? texel.rgb :
			floor((texel.rgb * texel.a + vertex6 * (31.0 - texel.a)) / 32.0));
	}
	if (textured && polygon_mode != 1) {
		float tex_alpha6 = ds_expand5(texel.a);
		float poly_alpha6 = ds_expand5(alpha5);
		alpha5 = min(31.0, floor(floor(((tex_alpha6 + 1.0) * (poly_alpha6 + 1.0) - 1.0) / 64.0) / 2.0));
	}
	return vec4(shade, alpha5);
}
