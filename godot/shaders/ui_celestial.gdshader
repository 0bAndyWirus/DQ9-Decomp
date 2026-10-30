shader_type canvas_item;

uniform bool reduced_motion = false;

float star(vec2 p, float scale, float t) {
	vec2 cell = floor(p * scale);
	vec2 q = fract(p * scale) - 0.5;
	float n = fract(sin(dot(cell, vec2(127.1, 311.7))) * 43758.5453);
	float glow = exp(-dot(q, q) * 120.0) * step(0.91, n);
	return glow * (0.45 + 0.35 * sin(t * 0.7 + n * 31.0));
}

float segment(vec2 p, vec2 a, vec2 b) {
	vec2 d = b - a;
	float h = clamp(dot(p - a, d) / dot(d, d), 0.0, 1.0);
	return exp(-length(p - a - h * d) * 600.0);
}

void fragment() {
	vec2 uv = UV;
	float t = reduced_motion ? 0.0 : TIME;
	vec3 col = mix(vec3(0.025, 0.045, 0.085), vec3(0.075, 0.12, 0.18), uv.y * 0.7);
	vec2 p = (uv - vec2(0.72, 0.46)) * vec2(1.4, 1.0);
	float a = t * 0.008;
	p = mat2(vec2(cos(a), -sin(a)), vec2(sin(a), cos(a))) * p;
	float radius = length(p);
	float rings = exp(-abs(radius - 0.30) * 500.0) + exp(-abs(radius - 0.39) * 500.0) + exp(-abs(radius - 0.48) * 500.0);
	float axes = exp(-abs(p.y) * 550.0) + exp(-abs(p.x) * 550.0);
	col += vec3(0.55, 0.42, 0.23) * (rings * 0.17 + axes * 0.05) * smoothstep(0.62, 0.25, radius);
	float constellation = segment(p, vec2(-0.25, -0.1), vec2(-0.11, -0.19));
	constellation += segment(p, vec2(-0.11, -0.19), vec2(0.05, -0.08));
	constellation += segment(p, vec2(0.05, -0.08), vec2(0.19, -0.17));
	col += vec3(0.50, 0.39, 0.21) * constellation * 0.14;
	col += vec3(0.75, 0.68, 0.50) * (star(uv, 48.0, t) + star(uv + 0.13, 83.0, t)) * 0.45;
	col += vec3(0.07, 0.08, 0.11) * exp(-dot(p, p) * 5.0);
	float vignette = smoothstep(1.0, 0.25, length((uv - 0.5) * vec2(1.0, 1.1)));
	col *= mix(0.55, 1.0, vignette);
	COLOR = vec4(col, 1.0);
}
