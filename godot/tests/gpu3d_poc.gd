extends SceneTree
## Synthetic, independently authored geometry/texture compatibility probe. Run with a rendered backend:
## xvfb-run -a Godot --path godot --script res://tests/gpu3d_poc.gd -- /absolute/output

const SIZE = Vector2i(256, 192)
const POLY_SOURCE = "res://shaders/ds_poly.gdshader"
const ID_SHADER = preload("res://shaders/ds_poly_id.gdshader")
const POST_SHADER = preload("res://shaders/ds_post.gdshader")
const Pixels = preload("res://tests/gpu3d_pixels.gd")
var failures: Array[String] = []
var results: Dictionary = {}
var output_dir: String
var gradient_image: Image
var gradient_texture: ImageTexture
var toon_image: Image
var toon_texture: ImageTexture
var fog_values: PackedInt32Array
var edge_colors: Array[Vector3] = []
var fog_texture: ImageTexture
var edge_texture: ImageTexture
var color_transfer: ImageTexture

func _initialize() -> void:
	call_deferred("_run")

func check(condition: bool, message: String) -> void:
	if not condition:
		failures.append(message)
		push_error(message)

func _polygon(points: Array, color: Color, id: int, z: float = 0.0) -> Dictionary:
	var count = points.size()
	var w: Array = []
	var depth: Array = []
	var colors: Array = []
	var uv: Array = []
	for i in count:
		w.append(1.0)
		depth.append(z)
		colors.append(color)
		uv.append(Vector2.ZERO)
	return {"points": points, "w": w, "z": depth, "colors": colors, "uv": uv,
		"id": id, "alpha": 31, "mode": 0, "textured": false, "fog": false,
		"priority": 0, "highlight": false, "alpha_test": false, "alpha_ref": 0}

func _quad(rect: Rect2, color: Color, id: int, z: float = 0.0) -> Dictionary:
	return _polygon([rect.position, rect.position + Vector2(rect.size.x, 0),
		rect.end, rect.position + Vector2(0, rect.size.y)], color, id, z)

func _data_texture(image: Image) -> ImageTexture:
	return ImageTexture.create_from_image(image)

func _make_tables() -> void:
	color_transfer = Pixels.color_transfer_texture()
	gradient_image = Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			gradient_image.set_pixel(x, y, Color(x / 63.0, y / 63.0, (63 if ((x / 8 + y / 8) % 2) == 0 else 16) / 63.0, 1.0))
	gradient_texture = _data_texture(gradient_image)
	toon_image = Image.create(32, 1, false, Image.FORMAT_RGBA8)
	for i in 32:
		toon_image.set_pixel(i, 0, Color((2 * i + 1) / 63.0, (63 - 2 * i) / 63.0, 16.0 / 63.0, 1))
	toon_texture = _data_texture(toon_image)
	var fog_image = Image.create(32, 1, false, Image.FORMAT_RGBA8)
	for i in 32:
		fog_values.append(mini(127, i * 4 + (3 if i == 31 else 0)))
		fog_image.set_pixel(i, 0, Color(fog_values[i] / 255.0, 0, 0, 1))
	fog_texture = _data_texture(fog_image)
	var edge_image = Image.create(8, 1, false, Image.FORMAT_RGBA8)
	for i in 8:
		var rgb = Vector3(63, 8, 8) if i == 1 else Vector3(8, 63, 16)
		edge_colors.append(rgb)
		edge_image.set_pixel(i, 0, Color(rgb.x / 63.0, rgb.y / 63.0, rgb.z / 63.0, 1))
	edge_texture = _data_texture(edge_image)

func _perspective_scene() -> Array:
	var flat = _polygon([Vector2(12, 18), Vector2(120, 28), Vector2(36, 174)], Color(8.0 / 63, 54.0 / 63, 16.0 / 63), 8, -0.4)
	var quad = _polygon([Vector2(136, 26), Vector2(242, 36), Vector2(224, 166), Vector2(128, 172)], Color.WHITE, 16)
	quad.w = [1.0, 3.0, 3.0, 1.0]
	quad.z = [-0.6, -0.2, -0.2, -0.6]
	quad.uv = [Vector2.ZERO, Vector2(1, 0), Vector2.ONE, Vector2(0, 1)]
	quad.textured = true
	return [flat, quad]

func _overlap_scene(reverse: bool = false) -> Array:
	var background = _quad(Rect2(12, 12, 232, 168), Color(4.0 / 63, 12.0 / 63, 20.0 / 63), 0, 0.7)
	var red = _quad(Rect2(28, 30, 140, 106), Color(1, 0, 0), 8, -0.2)
	var blue = _quad(Rect2(90, 66, 136, 100), Color(0, 0, 1), 16, -0.2)
	red.alpha = 15
	blue.alpha = 15
	red.priority = 40 if reverse else 20
	blue.priority = 20 if reverse else 40
	red.no_depth_write = true
	blue.no_depth_write = true
	return [background, red, blue]

func _fog_scene() -> Array:
	var background = _quad(Rect2(0, 0, 256, 192), Color(6.0 / 63, 8.0 / 63, 22.0 / 63), 0, 0.9)
	var quad = _quad(Rect2(24, 28, 164, 138), Color(58.0 / 63, 24.0 / 63, 8.0 / 63), 8)
	quad.z = [-0.85, 0.65, 0.65, -0.85]
	quad.fog = true
	var triangle = _polygon([Vector2(102, 68), Vector2(226, 84), Vector2(192, 158)], Color(10.0 / 63, 54.0 / 63, 18.0 / 63), 24, -0.7)
	triangle.fog = true
	return [background, quad, triangle]

func _new_viewport(scale: int) -> SubViewport:
	var viewport = SubViewport.new()
	viewport.size = SIZE * scale
	viewport.transparent_bg = true
	viewport.own_world_3d = true
	viewport.msaa_3d = Viewport.MSAA_DISABLED
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(viewport)
	var camera = Camera3D.new()
	camera.position = Vector3(0, 0, 6)
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 10
	camera.near = 0.1
	camera.far = 100
	viewport.add_child(camera)
	camera.current = true
	return viewport

func _material(polygon: Dictionary, metadata_pass: bool) -> ShaderMaterial:
	var material = ShaderMaterial.new()
	if metadata_pass:
		material.shader = ID_SHADER
	else:
		var shader = Shader.new()
		var defines = ""
		if polygon.get("additive", false): defines += "#define DS_ADDITIVE\n"
		elif polygon.get("no_depth_write", false): defines += "#define DS_NO_DEPTH_WRITE\n"
		if polygon.alpha == 31 and not polygon.get("texture_alpha", false) and not polygon.get("force_transparent", false): defines += "#define DS_OPAQUE\n"
		shader.code = defines + FileAccess.get_file_as_string(POLY_SOURCE)
		material.shader = shader
		material.render_priority = polygon.priority
	material.set_shader_parameter("ds_texture", polygon.get("texture", gradient_texture))
	material.set_shader_parameter("color_transfer", color_transfer)
	material.set_shader_parameter("display_rgb555", polygon.get("display_rgb555", false))
	material.set_shader_parameter("toon_table", toon_texture)
	material.set_shader_parameter("textured", polygon.textured)
	material.set_shader_parameter("polygon_mode", polygon.mode)
	material.set_shader_parameter("highlight", polygon.highlight)
	material.set_shader_parameter("polygon_alpha", polygon.alpha)
	material.set_shader_parameter("alpha_test_enabled", polygon.alpha_test)
	material.set_shader_parameter("alpha_test_ref", polygon.alpha_ref)
	material.set_shader_parameter("texture_repeat", polygon.get("repeat", 0))
	material.set_shader_parameter("texture_flip", polygon.get("flip", 0))
	material.set_shader_parameter("polygon_id", polygon.id)
	material.set_shader_parameter("polygon_fog", polygon.fog)
	material.set_shader_parameter("translucent_polygon", polygon.get("metadata_translucent", polygon.alpha < 31))
	material.set_shader_parameter("vertex_metadata", polygon.get("vertex_metadata", false))
	return material

func _mesh(polygons: Array, metadata_pass: bool) -> ArrayMesh:
	var mesh = ArrayMesh.new()
	for polygon in polygons:
		if polygon.alpha == 0: continue
		var vertices = PackedVector3Array()
		var uv = PackedVector2Array()
		var uv2 = PackedVector2Array()
		var colors = PackedColorArray()
		var indices = PackedInt32Array()
		for i in polygon.points.size():
			var point: Vector2 = polygon.points[i]
			var w: float = polygon.w[i]
			vertices.append(Vector3((point.x / 128.0 - 1.0) * w, (1.0 - point.y / 96.0) * w, polygon.z[i] * w))
			uv.append(polygon.uv[i])
			uv2.append(Vector2(w, 0))
			colors.append(polygon.colors[i])
		for i in range(1, polygon.points.size() - 1):
			indices.append_array(PackedInt32Array([0, i, i + 1]))
		var arrays: Array = []
		arrays.resize(Mesh.ARRAY_MAX)
		arrays[Mesh.ARRAY_VERTEX] = vertices
		arrays[Mesh.ARRAY_TEX_UV] = uv
		arrays[Mesh.ARRAY_TEX_UV2] = uv2
		arrays[Mesh.ARRAY_COLOR] = colors
		arrays[Mesh.ARRAY_INDEX] = indices
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {}, Mesh.ARRAY_FLAG_USE_DYNAMIC_UPDATE)
		mesh.surface_set_material(mesh.get_surface_count() - 1, _material(polygon, metadata_pass))
	return mesh

func _attach_mesh(viewport: SubViewport, mesh: Mesh) -> MeshInstance3D:
	var instance = MeshInstance3D.new()
	instance.mesh = mesh
	instance.custom_aabb = AABB(Vector3(-100, -100, -100), Vector3(200, 200, 200))
	instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	viewport.add_child(instance)
	return instance

func _settle() -> void:
	for i in 3: await process_frame
	await RenderingServer.frame_post_draw

func _save(image: Image, filename: String) -> void:
	check(image.save_png(output_dir.path_join(filename)) == OK, "Saving " + filename)

func _post(viewport: SubViewport, color: Texture2D, debug_depth: bool) -> ShaderMaterial:
	var material = ShaderMaterial.new()
	material.shader = POST_SHADER
	material.render_priority = 127
	material.set_shader_parameter("color_texture", color)
	material.set_shader_parameter("color_transfer", color_transfer)
	material.set_shader_parameter("fog_table", fog_texture)
	material.set_shader_parameter("edge_color_table", edge_texture)
	material.set_shader_parameter("disp3dcnt", 128 | 32)
	material.set_shader_parameter("fog_color", 20 | (24 << 5) | (31 << 10) | (31 << 16))
	material.set_shader_parameter("fog_offset", 2000)
	material.set_shader_parameter("debug_depth", debug_depth)
	var quad = QuadMesh.new()
	quad.size = Vector2(2, 2)
	quad.material = material
	_attach_mesh(viewport, quad)
	return material

func _capture(name: String, polygons: Array, scale: int, effects: bool = false) -> Image:
	var color_viewport = _new_viewport(scale)
	var mesh = _mesh(polygons, false)
	_attach_mesh(color_viewport, mesh)
	await _settle()
	var raw = color_viewport.get_texture().get_image()
	_save(raw, "%s_%dx_raw.png" % [name, scale])
	if not effects:
		color_viewport.queue_free()
		return raw
	var metadata_viewport = _new_viewport(scale)
	var id_instance = _attach_mesh(metadata_viewport, mesh)
	var surface = 0
	for polygon in polygons:
		if polygon.alpha == 0: continue
		id_instance.set_surface_override_material(surface, _material(polygon, true))
		surface += 1
	await _settle()
	_save(metadata_viewport.get_texture().get_image(), "%s_%dx_ids.png" % [name, scale])
	_post(metadata_viewport, color_viewport.get_texture(), false)
	await _settle()
	var final_image = metadata_viewport.get_texture().get_image()
	_save(final_image, "%s_%dx.png" % [name, scale])
	metadata_viewport.queue_free()
	color_viewport.queue_free()
	return final_image

func _edge(a: Vector2, b: Vector2, p: Vector2) -> float:
	return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)

func _top_left(a: Vector2, b: Vector2) -> bool:
	return b.y < a.y or (b.y == a.y and b.x > a.x)

func _round6(color: Color) -> Vector3:
	return Vector3(roundf(color.r * 63), roundf(color.g * 63), roundf(color.b * 63))

func _expand5(value: int) -> int:
	return 0 if value == 0 else value * 2 + 1

func _shade(polygon: Dictionary, uv: Vector2, color: Vector3) -> Color:
	var vertex = Vector3(roundf(color.x), roundf(color.y), roundf(color.z))
	var texel = Vector3(63, 63, 63)
	var ta = 31
	if polygon.textured:
		var image: Image = polygon.get("texture_image", gradient_image)
		var sample_value = image.get_pixel(clampi(int(floor(uv.x * image.get_width())), 0, image.get_width() - 1), clampi(int(floor(uv.y * image.get_height())), 0, image.get_height() - 1))
		texel = _round6(sample_value)
		ta = int(roundf(sample_value.a * 31))
	var toon = vertex
	if polygon.mode == 2: toon = _round6(toon_image.get_pixel(int(vertex.x) >> 1, 0))
	var shade = toon if polygon.mode == 2 and not polygon.highlight else vertex
	var alpha: int = polygon.alpha
	if polygon.mode == 0 or polygon.mode == 2:
		for i in 3:
			var modulation = vertex.x if polygon.highlight and polygon.mode == 2 else shade[i]
			var value = floor(((texel[i] + 1) * (modulation + 1) - 1) / 64.0)
			shade[i] = minf(63, value + toon[i]) if polygon.highlight and polygon.mode == 2 else value
	elif polygon.textured:
		shade = vertex if ta == 0 else (texel if ta == 31 else (texel * ta + vertex * (31 - ta)) / 32.0).floor()
	if polygon.textured and polygon.mode != 1:
		alpha = int(floor(floor(((_expand5(ta) + 1) * (_expand5(alpha) + 1) - 1) / 64.0) / 2.0))
	if alpha == 0 or (polygon.alpha_test and alpha < polygon.alpha_ref): return Color(0, 0, 0, 0)
	return Color(shade.x / 63, shade.y / 63, shade.z / 63, alpha / 31.0)

func _reference(polygons: Array, affine: bool = false, effects: bool = false) -> Image:
	var image = Image.create(256, 192, false, Image.FORMAT_RGBA8)
	var depths = PackedInt32Array()
	depths.resize(256 * 192)
	depths.fill(16777215)
	var ids = PackedInt32Array()
	ids.resize(256 * 192)
	var fog = PackedByteArray()
	fog.resize(256 * 192)
	for polygon in polygons:
		for triangle in range(1, polygon.points.size() - 1):
			var indices = [0, triangle, triangle + 1]
			var a: Vector2 = polygon.points[indices[0]]
			var b: Vector2 = polygon.points[indices[1]]
			var c: Vector2 = polygon.points[indices[2]]
			var area = _edge(a, b, c)
			for y in range(maxi(0, int(floor(minf(a.y, minf(b.y, c.y))))), mini(192, int(ceil(maxf(a.y, maxf(b.y, c.y)))))):
				for x in range(maxi(0, int(floor(minf(a.x, minf(b.x, c.x))))), mini(256, int(ceil(maxf(a.x, maxf(b.x, c.x)))))):
					var p = Vector2(x, y) # DS integer sampling, after the shader's half-pixel nudge.
					var weights = Vector3(_edge(b, c, p), _edge(c, a, p), _edge(a, b, p)) / area
					if minf(weights.x, minf(weights.y, weights.z)) < 0: continue
					if weights.x == 0 and not _top_left(b, c): continue
					if weights.y == 0 and not _top_left(c, a): continue
					if weights.z == 0 and not _top_left(a, b): continue
					var inv_w = 0.0
					var uv = Vector2.ZERO
					var color = Vector3.ZERO
					var z = 0.0
					for i in 3:
						var index: int = indices[i]
						var factor: float = weights[i] / (1.0 if affine else polygon.w[index])
						inv_w += factor
						uv += polygon.uv[index] * factor
						color += _round6(polygon.colors[index]) * factor
						z += polygon.z[index] * weights[i]
					var depth = int(floor(clampf((z + 1.0) * 0.5, 0, 1) * 4194303)) * 4
					var offset = y * 256 + x
					if depth > depths[offset]: continue
					var shade = _shade(polygon, uv / inv_w, color / inv_w)
					if shade.a == 0: continue
					image.set_pixel(x, y, shade)
					depths[offset] = depth
					ids[offset] = polygon.id
					fog[offset] = 1 if polygon.fog else 0
	if effects:
		for y in 192:
			for x in 256:
				var offset = y * 256 + x
				var source = image.get_pixel(x, y)
				if source.a == 0: continue
				var rgb = _round6(source)
				for direction in [Vector2i.RIGHT, Vector2i.DOWN, Vector2i.LEFT, Vector2i.UP]:
					var neighbor = Vector2i(x, y) + direction
					if neighbor.x < 0 or neighbor.x >= 256 or neighbor.y < 0 or neighbor.y >= 192:
						if ids[offset] != 0 and depths[offset] < 16777215:
							rgb = edge_colors[ids[offset] >> 3]
							break
					else:
						var neighbor_offset = neighbor.y * 256 + neighbor.x
						if ids[offset] != ids[neighbor_offset] and depths[offset] >= depths[neighbor_offset]:
							rgb = edge_colors[ids[neighbor_offset] >> 3]
							break
				if fog[offset] != 0:
					var relative = maxi(0, (depths[offset] >> 9) - 2000)
					var density: int
					if relative <= 1024: density = fog_values[0]
					elif relative >= 32768: density = fog_values[31]
					else:
						var segment = relative / 1024
						var fraction = relative % 1024
						density = (fog_values[segment - 1] * (1024 - fraction) + fog_values[segment] * fraction) / 1024
					var weight = 128 if density == 127 else density
					rgb = ((Vector3(41, 49, 63) * weight + rgb * (128 - weight)) / 128.0).floor()
				image.set_pixel(x, y, Color(rgb.x / 63, rgb.y / 63, rgb.z / 63, source.a))
	return image

func _compare(actual: Image, expected: Image, mask: PackedByteArray = PackedByteArray()) -> Dictionary:
	var total_error = 0.0
	var compared = 0
	var coverage_errors = 0
	var bad = 0
	var maximum = 0.0
	var alpha_error := 0.0
	var alpha_max := 0.0
	for y in 192:
		for x in 256:
			if not mask.is_empty() and mask[y * 256 + x] == 0: continue
			var a = actual.get_pixel(x, y)
			var b = expected.get_pixel(x, y)
			if (a.a > 0) != (b.a > 0): coverage_errors += 1
			if b.a == 0: continue
			var alpha_delta := absf(a.a - b.a) * 255.0
			alpha_error += alpha_delta
			alpha_max = maxf(alpha_max, alpha_delta)
			var error = Vector3(absf(a.r - b.r), absf(a.g - b.g), absf(a.b - b.b)) * 255.0
			total_error += error.x + error.y + error.z
			maximum = maxf(maximum, maxf(error.x, maxf(error.y, error.z)))
			if maxf(error.x, maxf(error.y, error.z)) > 8.01: bad += 1
			compared += 1
	return {"pixels": compared, "coverage_errors": coverage_errors, "mae_levels": total_error / maxf(1, 3 * compared),
		"max_levels": maximum, "pixels_over_8": bad, "percent_over_8": 100.0 * bad / maxf(1, compared),
		"alpha_mae_levels": alpha_error / maxf(1, compared), "alpha_max_levels": alpha_max}

func _depth_probe() -> void:
	var polygons: Array = []
	for i in 4:
		polygons.append(_quad(Rect2(8 + i * 62, 24, 56, 144), Color.WHITE, i * 8, [-0.75, -0.25, 0.25, 0.75][i]))
	var viewport = _new_viewport(1)
	_attach_mesh(viewport, _mesh(polygons, true))
	_post(viewport, gradient_texture, true)
	await _settle()
	var image = viewport.get_texture().get_image()
	_save(image, "depth_probe.png")
	var values: Array = []
	for i in 4:
		var value = image.get_pixel(32 + i * 62, 90).r
		values.append(value)
		check(absf(value - ([-0.75, -0.25, 0.25, 0.75][i] + 1.0) * 0.5) < 2.0 / 255, "Opaque depth texture retains overridden DS clip z")
	results.depth_probe = values
	viewport.queue_free()

func _metadata_probe() -> void:
	var polygons: Array = []
	for flags in 4:
		for id in 64:
			var index = flags * 64 + id
			var polygon = _quad(Rect2((index % 16) * 16, (index / 16) * 12, 16, 12), Color.WHITE, id)
			polygon.alpha = id % 31 + 1
			polygon.fog = (flags & 1) != 0
			polygon.metadata_translucent = (flags & 2) != 0
			polygons.append(polygon)
	var viewport = _new_viewport(1)
	_attach_mesh(viewport, _mesh(polygons, true))
	await _settle()
	var image = viewport.get_texture().get_image()
	_save(image, "metadata_roundtrip.png")
	var errors = 0
	for index in polygons.size():
		var sample_value = image.get_pixel((index % 16) * 16 + 8, (index / 16) * 12 + 6)
		var id = int(roundf((sample_value.r * 255 - 64) / 3))
		var flags = int(roundf((sample_value.g * 255 - 64) / 48))
		var alpha = int(roundf((sample_value.b * 255 - 64) / 6))
		if id != index % 64 or flags != index / 64 or alpha != polygons[index].alpha: errors += 1
	results.metadata_roundtrip = {"cases": 256, "errors": errors}
	check(errors == 0, "All polygon IDs/flag combinations survive Compatibility color conversion")
	viewport.queue_free()

func _vertex_metadata_probe() -> void:
	# One surface deliberately mixes every ID/flag combination. High alpha bits
	# also verify UV2's storage does not round away IDs in the packed float.
	var vertices = PackedVector3Array()
	var uv2 = PackedVector2Array()
	var uv = PackedVector2Array()
	var colors = PackedColorArray()
	for index in 256:
		var x = (index % 16) * 16
		var y = (index / 16) * 12
		var corners = [Vector2(x, y), Vector2(x + 16, y), Vector2(x + 16, y + 12), Vector2(x, y + 12)]
		var id = index % 64
		var flags = index / 64
		var alpha = id % 31 + 1
		var packed = id | (flags << 6) | (alpha << 8)
		for corner in [0, 1, 2, 0, 2, 3]:
			var point: Vector2 = corners[corner]
			vertices.append(Vector3(point.x / 128.0 - 1.0, 1.0 - point.y / 96.0, 0))
			uv2.append(Vector2(1, packed))
			uv.append(Vector2.ZERO)
			colors.append(Color.WHITE)
	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_TEX_UV2] = uv2
	arrays[Mesh.ARRAY_TEX_UV] = uv
	arrays[Mesh.ARRAY_COLOR] = colors
	var mesh = ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var polygon = _quad(Rect2(0, 0, 256, 192), Color.WHITE, 0)
	polygon.vertex_metadata = true
	mesh.surface_set_material(0, _material(polygon, true))
	var viewport = _new_viewport(1)
	_attach_mesh(viewport, mesh)
	await _settle()
	var image = viewport.get_texture().get_image()
	_save(image, "vertex_metadata_roundtrip.png")
	var errors = 0
	for index in 256:
		var sample_value = image.get_pixel((index % 16) * 16 + 8, (index / 16) * 12 + 6)
		var id = int(roundf((sample_value.r * 255 - 64) / 3))
		var flags = int(roundf((sample_value.g * 255 - 64) / 48))
		var alpha = int(roundf((sample_value.b * 255 - 64) / 6))
		if id != index % 64 or flags != index / 64 or alpha != (index % 64) % 31 + 1: errors += 1
	results.vertex_metadata = {"cases": 256, "surfaces": 1, "errors": errors}
	check(errors == 0, "Packed UV2.y metadata survives one surface with mixed polygon IDs/fog/alpha")
	viewport.queue_free()


func _camera_probe() -> void:
	var expected = await _capture("camera_default", _perspective_scene(), 1)
	var viewport = _new_viewport(1)
	_attach_mesh(viewport, _mesh(_perspective_scene(), false))
	for child in viewport.get_children():
		if child is Camera3D:
			child.position = Vector3(20, 10, 6)
			child.rotation = Vector3(0.3, 0.6, 0.1)
			child.size = 3
	await _settle()
	var actual = viewport.get_texture().get_image()
	_save(actual, "camera_moved.png")
	results.camera_override_identical = actual.get_data() == expected.get_data()
	check(results.camera_override_identical, "POSITION bypasses camera projection and transforms")
	viewport.queue_free()

func _fog_density(depth15: int, offset: int, shift: int) -> int:
	var step_size = 1024 >> shift
	if step_size == 0: return fog_values[0 if depth15 <= offset else 31]
	var relative = maxi(0, depth15 - offset)
	if relative <= step_size: return fog_values[0]
	if relative >= step_size * 32: return fog_values[31]
	var segment = relative / step_size
	var fraction = relative % step_size
	return (fog_values[segment - 1] * (step_size - fraction) + fog_values[segment] * fraction) / step_size

func _fog_formula_probe() -> void:
	var polygons: Array = []
	var rgb = Vector3(48, 32, 16)
	var z_values = [-0.99, -0.75, 0.5, 0.99]
	for i in 4:
		var polygon = _quad(Rect2(i * 64, 0, 64, 192), Color(rgb.x / 63, rgb.y / 63, rgb.z / 63), 8, z_values[i])
		polygon.fog = true
		polygons.append(polygon)
	var color_viewport = _new_viewport(1)
	_attach_mesh(color_viewport, _mesh(polygons, false))
	var metadata_viewport = _new_viewport(1)
	_attach_mesh(metadata_viewport, _mesh(polygons, true))
	var material = _post(metadata_viewport, color_viewport.get_texture(), false)
	var cases = [{"name": "shift0", "shift": 0, "offset": 2000, "alpha_only": false},
		{"name": "shift2", "shift": 2, "offset": 2000, "alpha_only": false},
		{"name": "step0", "shift": 15, "offset": 16000, "alpha_only": false},
		{"name": "alpha_only", "shift": 0, "offset": 2000, "alpha_only": true},
		{"name": "small_rgb555", "shift": 2, "offset": 2000, "alpha_only": false,
			"fog_color": 1 | (4 << 5) | (7 << 10) | (31 << 16)}]
	var maximum = 0.0
	var maximum_small = 0.0
	for fog_case in cases:
		material.set_shader_parameter("disp3dcnt", 128 | (fog_case.shift << 8) | (64 if fog_case.alpha_only else 0))
		material.set_shader_parameter("fog_offset", fog_case.offset)
		var raw_color: int = fog_case.get("fog_color", 20 | (24 << 5) | (31 << 10) | (31 << 16))
		material.set_shader_parameter("fog_color", raw_color)
		var fog_rgb = Vector3(_expand5(raw_color & 31), _expand5((raw_color >> 5) & 31), _expand5((raw_color >> 10) & 31))
		await _settle()
		var image = metadata_viewport.get_texture().get_image()
		_save(image, "fog_formula_%s.png" % fog_case.name)
		for i in 4:
			var depth15 = (int(floor((z_values[i] + 1) * 0.5 * 4194303)) * 4) >> 9
			var density = _fog_density(depth15, fog_case.offset, fog_case.shift)
			var weight = 128 if density == 127 else density
			var expected = rgb if fog_case.alpha_only else ((fog_rgb * weight + rgb * (128 - weight)) / 128.0).floor()
			var actual = image.get_pixel(i * 64 + 32, 90)
			var error = (Vector3(actual.r, actual.g, actual.b) * 255 - expected * (255.0 / 63)).abs()
			if fog_case.has("fog_color"):
				maximum_small = maxf(maximum_small, maxf(error.x, maxf(error.y, error.z)))
			else:
				maximum = maxf(maximum, maxf(error.x, maxf(error.y, error.z)))
	results.fog_formula = {"cases": 20, "max_error_levels": maximum, "small_rgb555_max_error_levels": maximum_small}
	# The transfer table preserves dark RGB6; only byte expansion rounding remains.
	check(maximum <= 1.01 and maximum_small <= 1.01, "Fog shifts, step=0, endpoints, density127, alpha-only and dark RGB555 expansion follow CPU formula")
	metadata_viewport.queue_free()
	color_viewport.queue_free()

func _alpha_probe() -> void:
	var polygons: Array = []
	var expected_values = PackedInt32Array()
	for ta in 32:
		var texture_image = Image.create(1, 1, false, Image.FORMAT_RGBA8)
		texture_image.set_pixel(0, 0, Color(1, 1, 1, ta / 31.0))
		var texture = _data_texture(texture_image)
		for pa in range(1, 32):
			var polygon = _quad(Rect2((pa - 1) * 8, ta * 6, 8, 6), Color.WHITE, 0)
			polygon.alpha = pa
			polygon.textured = true
			polygon.texture = texture
			polygons.append(polygon)
			expected_values.append(int(floor(floor(((_expand5(ta) + 1) * (_expand5(pa) + 1) - 1) / 64.0) / 2.0)))
	var viewport = _new_viewport(1)
	# ArrayMesh has a measured hard cap of 256 surfaces in Godot 4.5.
	for begin in range(0, polygons.size(), 256):
		_attach_mesh(viewport, _mesh(polygons.slice(begin, mini(begin + 256, polygons.size())), true))
	await _settle()
	var image = viewport.get_texture().get_image()
	_save(image, "alpha_roundtrip.png")
	var errors = 0
	for i in polygons.size():
		var sample_value = image.get_pixel((i % 31) * 8 + 4, (i / 31) * 6 + 3)
		var alpha = 0 if sample_value.a == 0 else int(roundf((sample_value.b * 255 - 64) / 6))
		if alpha != expected_values[i]: errors += 1
	results.alpha_roundtrip = {"cases": 992, "errors": errors}
	check(errors == 0, "All non-wireframe polygon-alpha/texel-alpha products use native expand6")
	viewport.queue_free()

func _wrap_coordinate(value: int, repeats: bool, flips: bool) -> int:
	if not repeats: return clampi(value, 0, 63)
	var period = 128 if flips else 64
	var wrapped = posmod(value, period)
	return period - 1 - wrapped if flips and wrapped >= 64 else wrapped

func _sampler_probe() -> void:
	var coordinates = [-72, -65, -64, -63, -1, 0, 1, 63, 64, 65, 72, 127, 128]
	var polygons: Array = []
	for mode in 4:
		for i in coordinates.size():
			var polygon = _quad(Rect2(i * 18, 8 + mode * 44, 16, 40), Color.WHITE, 0)
			var uv = Vector2(8, coordinates[i]) / 64.0 if mode == 3 else Vector2(coordinates[i], 8) / 64.0
			polygon.uv = [uv, uv, uv, uv]
			polygon.textured = true
			polygon.repeat = 0 if mode == 0 else 3
			polygon.flip = 1 if mode == 2 else (2 if mode == 3 else 0)
			polygons.append(polygon)
	var image = await _capture("sampler", polygons, 1)
	var errors = 0
	var maximum = 0.0
	for mode in 4:
		for i in coordinates.size():
			var x = 8 if mode == 3 else coordinates[i]
			var y = coordinates[i] if mode == 3 else 8
			x = _wrap_coordinate(x, mode != 0, mode == 2)
			y = _wrap_coordinate(y, mode != 0, mode == 3)
			var expected = gradient_image.get_pixel(x, y)
			var actual = image.get_pixel(i * 18 + 8, 28 + mode * 44)
			var error = Vector3(absf(actual.r - expected.r), absf(actual.g - expected.g), absf(actual.b - expected.b)) * 255
			var max_error = maxf(error.x, maxf(error.y, error.z))
			if max_error > 4.01: errors += 1
			maximum = maxf(maximum, max_error)
	results.sampler = {"cases": 52, "errors": errors, "max_error_levels": maximum}
	check(errors == 0, "Clamp/repeat/flip sample integer DS texels including negative/boundary UVs")



func _depth_and_blend_probe() -> void:
	var background = _quad(Rect2(0, 0, 256, 192), Color(0, 0, 1), 0, 0.25)
	var polygons: Array = [background]
	for i in 3:
		var polygon = _quad(Rect2(8 + i * 82, 24, 72, 144), Color(1, 0, 0), (i + 1) * 8, [0.25, -0.5, 0.75][i])
		polygon.force_transparent = true
		polygon.priority = 20
		polygons.append(polygon)
	var image = await _capture("depth_compare", polygons, 1)
	check(image.get_pixel(40, 90).r > 0.99, "Equal DS Z passes the hardware depth test")
	check(image.get_pixel(122, 90).r > 0.99, "Nearer DS Z passes the hardware depth test")
	check(image.get_pixel(204, 90).b > 0.99, "Farther DS Z fails the hardware depth test")
	var viewport = _new_viewport(1)
	var translucent = _quad(Rect2(16, 16, 224, 160), Color(1, 0, 0), 8, -0.75)
	translucent.alpha = 15
	_attach_mesh(viewport, _mesh([translucent], false))
	_post(viewport, gradient_texture, true)
	await _settle()
	var depth_image = viewport.get_texture().get_image()
	_save(depth_image, "transparent_depth_probe.png")
	results.transparent_depth_sample = depth_image.get_pixel(120, 90).r
	check(results.transparent_depth_sample > 0.99, "Depth snapshot excludes ALPHA-writing geometry even with depth_draw_always")
	viewport.queue_free()
	var base = _quad(Rect2(0, 0, 256, 192), Color(0, 0, 20.0 / 63), 0, 0.5)
	var additive = _quad(Rect2(16, 16, 224, 160), Color(1, 0, 0), 8, -0.5)
	additive.alpha = 15
	additive.additive = true
	var added = await _capture("additive", [base, additive], 1)
	var sample_value = added.get_pixel(120, 90)
	results.additive_sample = [sample_value.r, sample_value.g, sample_value.b, sample_value.a]
	check(absf(sample_value.r - 15.0 / 31) < 4.0 / 255 and absf(sample_value.b - 20.0 / 63) < 4.0 / 255, "blend_add material variant adds alpha-weighted source")


func _mode_probe() -> void:
	var texture_image = Image.create(1, 1, false, Image.FORMAT_RGBA8)
	texture_image.set_pixel(0, 0, Color(48.0 / 63, 12.0 / 63, 32.0 / 63, 15.0 / 31))
	var texture = _data_texture(texture_image)
	var polygons: Array = []
	for i in 6:
		var polygon = _quad(Rect2(6 + i * 41, 20, 36, 152), Color(24.0 / 63, 42.0 / 63, 8.0 / 63), i * 8)
		polygon.textured = true
		polygon.texture = texture
		polygon.texture_image = texture_image
		polygon.mode = [0, 1, 2, 2, 0, 0][i]
		polygon.highlight = i == 3
		polygon.alpha_test = i >= 4
		polygon.alpha_ref = 15 if i == 4 else 16
		# Opaque variant deliberately isolates RGB arithmetic from GL alpha blending.
		polygons.append(polygon)
	var actual = await _capture("modes", polygons, 1)
	var expected = _reference(polygons)
	for y in 192:
		for x in 256:
			var pixel = expected.get_pixel(x, y)
			if pixel.a > 0:
				pixel.a = 1
				expected.set_pixel(x, y, pixel)
	_save(expected, "modes_cpu.png")
	var comparison = _compare(actual, expected)
	results.modes = comparison
	check(comparison.mae_levels < 4 and comparison.percent_over_8 < 5 and comparison.coverage_errors == 0, "Modulate/decal/toon/highlight and alpha-test RGB/coverage")

func _color_transfer_probe() -> void:
	var errors := [0, 0]
	for display in 2:
		var polygons: Array = []
		for level in 64:
			var polygon := _quad(Rect2((level % 8) * 32, (level / 8) * 24, 32, 24),
				Color(level / 63.0, level / 63.0, level / 63.0), 0)
			polygon["display_rgb555"] = display != 0
			polygons.append(polygon)
		var image := await _capture("color_transfer_%d" % display, polygons, 1)
		var bytes := image.get_data()
		for level in 64:
			var offset := (((level / 8) * 24 + 12) * 256 + (level % 8) * 32 + 16) * 4
			var expected := (level << 2) | (level >> 4)
			if display != 0:
				var rgb5 := level >> 1
				expected = (rgb5 << 3) | (rgb5 >> 2)
			for channel in 3:
				if bytes[offset + channel] != expected: errors[display] += 1
	results.color_transfer = {"rgb6_levels": 64, "channel_errors": errors[0], "lcd_channel_errors": errors[1]}
	check(errors[0] == 0 and errors[1] == 0, "Every native RGB6 and final RGB555 level survives the spatial transfer exactly")

func _native_raster_probe() -> void:
	results.native_reference_available = ClassDB.class_exists("DqGpuSceneFixture")
	if not results.native_reference_available: return
	var fixture: Node = ClassDB.instantiate("DqGpuSceneFixture")
	root.add_child(fixture)
	var cases: Array = [{"name": "flat"}, {"name": "clear_plane", "clear_alpha": 31},
		{"name": "partial_clear_edges", "clear_alpha": 16, "edges": true},
		{"name": "partial_clear_aa", "clear_alpha": 16, "edges": true, "antialias": true},
		{"name": "empty_clear_fog", "clear_alpha": 0, "clear_fog": true, "fog": true},
		{"name": "partial_clear_fog", "clear_alpha": 16, "clear_fog": true, "fog": true},
		{"name": "partial_clear_alpha_fog", "clear_alpha": 16, "clear_fog": true, "fog": true, "alpha_only": true},
		{"name": "binary_a3_edges", "format": 1, "clear_alpha": 16, "edges": true},
		{"name": "binary_a5_fog", "format": 6, "clear_alpha": 16, "clear_fog": true, "fog": true, "polygon_fog": true},
		{"name": "filtered_a3_edges", "format": 1, "partial_texture": true, "alpha_ref": 28, "clear_alpha": 16, "edges": true},
		{"name": "partial_a3", "format": 1, "partial_texture": true},
		{"name": "fog_alpha16_zero", "clear_alpha": 16, "clear_fog": true, "fog": true, "polygon_fog": true, "fog_alpha": 16, "fog_density": 0},
		{"name": "fog_alpha16_full", "clear_alpha": 16, "clear_fog": true, "fog": true, "polygon_fog": true, "fog_alpha": 16},
		{"name": "vertex", "varying_color": true},
		{"name": "toon", "format": 7, "mode": 2, "varying_color": true},
		{"name": "highlight", "format": 7, "mode": 2, "highlight": true, "varying_color": true},
		{"name": "decal", "format": 7, "mode": 1}, {"name": "alpha15", "alpha": 15},
		{"name": "perspective", "format": 7, "perspective": true, "varying_color": true},
		{"name": "quad", "format": 7, "quad": true, "varying_color": true},
		{"name": "repeat_flip", "format": 7, "wrap": 15}]
	for format in range(1, 8): cases.append({"name": "format%d" % format, "format": format})
	var metrics: Dictionary = {}
	for options in cases:
		var stats: Dictionary = fixture.render_probe(options)
		check(int(stats.polygon_count) == 1, "native probe assembles exactly one GX polygon")
		if options.name in ["binary_a3_edges", "binary_a5_fog", "filtered_a3_edges"]:
			check(bool(stats.classified_translucent) and not bool(stats.fragment_blending),
				"Binary/alpha-tested texels use opaque depth while preserving DS-translucent edge flags")
			var mesh: MeshInstance3D = fixture.get_scene().get_node("Gpu3DViewport/Gpu3DPolygons")
			check(mesh.mesh.surface_get_material(1).shader.code.begins_with("#define DS_OPAQUE"),
				"Proven opaque texels use the opaque shader, not depth-never transparent shading")
		if options.name in ["alpha15", "partial_a3"]:
			check(bool(stats.fragment_blending), "Native partial alpha is classified for software-only footprints")
		await _settle()
		var scene: Node = fixture.get_scene()
		var actual := Pixels.straight_alpha(scene.get_texture().get_image())
		var expected: Image = Pixels.rgb555_display_image(scene.get_reference_image())
		_save(actual, "native_%s_gpu.png" % options.name)
		_save(expected, "native_%s_cpu.png" % options.name)
		var comparison := _compare(actual, expected)
		var mask: PackedByteArray = scene.get_raw_mask_texture().get_image().get_data()
		comparison["opaque_mask"] = _compare(actual, expected, mask)
		metrics[options.name] = comparison
		print("GPU3D_NATIVE_PROBE %s %s" % [options.name, JSON.stringify(comparison)])
		if not OS.get_cmdline_user_args().has("--record-only") and options.name not in ["alpha15", "partial_a3"]:
			check(comparison.mae_levels < 4 and comparison.percent_over_8 < 5,
				"native edge-walker shading probe %s: %s" % [options.name, comparison])
		if options.name == "alpha15":
			check(int(stats.gpu_pixels) == 0 and int(stats.mesh_polygon_count) == 0,
				"Partial polygon alpha remains entirely software, with no GPU colour/depth contribution")
		if comparison.opaque_mask.pixels > 0:
			check(comparison.opaque_mask.mae_levels < 4 and comparison.opaque_mask.percent_over_8 < 5 and
				comparison.opaque_mask.alpha_mae_levels < 1 and comparison.opaque_mask.coverage_errors <= 1,
				"Native opaque-mask probe %s: %s" % [options.name, comparison])
		if options.name in ["flat", "quad", "clear_plane"]:
			# Sparse native samples can omit a fractional polygon tip. Use the
			# exact projected28.4 perimeter, not a padded software-sample box.
			var bounds: Vector4 = stats.projected_bounds
			var clear := int(options.get("clear_alpha", 0)) != 0
			var nudge := 0.0 if clear else 0.5
			var margin := 0.0 if clear else 1.0 / 32.0
			options["scale"] = 3
			fixture.render_probe(options)
			await _settle()
			var enlarged: Image = scene.get_raw_texture().get_image()
			_save(enlarged, "native_%s_3x_gpu.png" % options.name)
			var escaped := 0
			var transparent := 0
			var allowed := Rect2((Vector2(bounds.x, bounds.y) + Vector2.ONE * (nudge - margin)) * 3,
				(Vector2(bounds.z - bounds.x, bounds.w - bounds.y) + Vector2.ONE * (margin * 2)) * 3)
			for y in range(enlarged.get_height()):
				for x in range(enlarged.get_width()):
					if enlarged.get_pixel(x, y).a == 0: transparent += 1
					elif not allowed.has_point(Vector2(x + 0.5, y + 0.5)): escaped += 1
			check(escaped == 0, "3x geometry stays inside its projected perimeter/nudge/margin bounds: %s/%d" % [options.name, escaped])
			if options.name == "clear_plane":
				check(transparent == 0, "3x clear buffer fills every subpixel, without the polygon phase nudge")
			metrics[options.name]["hires_escaped_pixels"] = escaped
			metrics[options.name]["hires_transparent_pixels"] = transparent
	fixture.free()
	results.native_raster = metrics

func _single_layer_probe() -> void:
	if not ClassDB.class_exists("DqGpuSceneFixture"): return
	var fixture: Node = ClassDB.instantiate("DqGpuSceneFixture")
	root.add_child(fixture)
	var rng := RandomNumberGenerator.new()
	rng.seed = 0x9d517
	var measurements: Array = []
	for i in 64:
		var options := {"name": "single_layer_%d" % i, "blend": true,
			"clear_alpha": 31, "alpha": 1 + i % 30, "color": rng.randi_range(0, 32767)}
		if i >= 30:
			options["format"] = 1
			options["partial_texture"] = true
		if i >= 60:
			options["alpha"] = 31
			options["edges"] = (i & 1) != 0
			options["fog"] = (i & 2) != 0
			options["clear_fog"] = true
			options["polygon_fog"] = true
		var stats: Dictionary = fixture.render_probe(options)
		for ignored in 3: await process_frame
		await RenderingServer.frame_post_draw
		var scene: Node = fixture.get_scene()
		var actual := Pixels.straight_alpha(scene.get_texture().get_image())
		var reference := Pixels.rgb555_display_image(scene.get_reference_image())
		var mask: PackedByteArray = scene.get_mask_texture().get_image().get_data()
		var comparison := _compare(actual, reference, mask)
		comparison["case"] = i
		measurements.append(comparison)
		check(int(stats.gpu_pixels) == 49152 and int(stats.mesh_polygon_count) == 1,
			"One partial layer over opaque clear is GPU-owned, case %d" % i)
		check(comparison.mae_levels < 4 and comparison.percent_over_8 < 5,
			"Native integer blend reference on random RGB/alpha, case %d: %s" % [i, comparison])
		if i in [14, 44]:
			_save(actual, "single_layer_%d_gpu.png" % i)
			_save(reference, "single_layer_%d_cpu.png" % i)
	fixture.free()
	results.single_layer = measurements


func _run() -> void:
	var arguments = OS.get_cmdline_user_args()
	if arguments.has("--probe-mrt"):
		var shader = Shader.new()
		shader.code = "shader_type spatial;\nlayout(location = 1) out vec4 polygon_id;\nvoid fragment() { ALBEDO = vec3(1.0); polygon_id = vec4(1.0); }\n"
		var material = ShaderMaterial.new()
		material.shader = shader
		var viewport = _new_viewport(1)
		var mesh = QuadMesh.new()
		mesh.material = material
		_attach_mesh(viewport, mesh)
		await _settle()
		print("MRT_PROBE: shader compilation diagnostic above is expected; no public spatial MRT output")
		quit(0)
		return
	output_dir = arguments[0] if not arguments.is_empty() else ProjectSettings.globalize_path("res://../build/gpu3d-poc")
	check(DirAccess.make_dir_recursive_absolute(output_dir) == OK, "Creating screenshot directory")
	results.renderer = RenderingServer.get_current_rendering_method()
	results.adapter = RenderingServer.get_video_adapter_name()
	check(results.renderer == "gl_compatibility", "POC must exercise gl_compatibility")
	for property in ShaderMaterial.new().get_property_list():
		if property.name == "render_priority": results.render_priority_range = property.hint_string
	results.rendering_device_available = RenderingServer.get_rendering_device() != null
	_make_tables()
	for scale in [1, 3]:
		var perspective = await _capture("perspective", _perspective_scene(), scale)
		var overlap = await _capture("overlap", _overlap_scene(), scale)
		var fog = await _capture("fog_edges", _fog_scene(), scale, true)
		if scale == 1:
			var expected = _reference(_perspective_scene())
			_save(expected, "perspective_cpu.png")
			results.perspective = _compare(perspective, expected)
			results.affine_control = _compare(perspective, _reference(_perspective_scene(), true))
			check(results.perspective.coverage_errors <= 4, "Integer-sampled GL barycentric control has at most four boundary-tie pixels; native edge-walker probes below are the DS reference")
			check(results.perspective.mae_levels < 4 and results.perspective.percent_over_8 < 5, "Perspective UV gradient matches independent CPU barycentrics")
			check(results.affine_control.mae_levels > results.perspective.mae_levels + 8, "Varyings genuinely use clip w, not affine UVs")
			var reverse = await _capture("overlap_reverse", _overlap_scene(true), 1)
			var normal_pixel = overlap.get_pixel(120, 90)
			var reverse_pixel = reverse.get_pixel(120, 90)
			results.overlap_normal = [normal_pixel.r, normal_pixel.g, normal_pixel.b, normal_pixel.a]
			results.overlap_reverse = [reverse_pixel.r, reverse_pixel.g, reverse_pixel.b, reverse_pixel.a]
			check(normal_pixel.b > normal_pixel.r and reverse_pixel.r > reverse_pixel.b, "Transparent render_priority controls surface blend order")
			var a = 15.0 / 31
			var background = Color(4.0 / 63, 12.0 / 63, 20.0 / 63)
			var first = Color(1, 0, 0) * a + background * (1 - a)
			var reference = Color(0, 0, 1) * a + first * (1 - a)
			results.overlap_reference = [reference.r, reference.g, reference.b]
			check(maxf(absf(normal_pixel.r - reference.r), maxf(absf(normal_pixel.g - reference.g), absf(normal_pixel.b - reference.b))) < 4.0 / 255, "Translucent overlap agrees with standard GPU alpha blend reference")
			var fog_cpu = _reference(_fog_scene(), false, true)
			_save(fog_cpu, "fog_edges_cpu.png")
			results.fog_edges = _compare(fog, fog_cpu)
			check(results.fog_edges.mae_levels < 4 and results.fog_edges.percent_over_8 < 5, "Opaque Z fog and polygon-ID edges match DS CPU formula")
	await _depth_probe()
	await _mode_probe()
	await _metadata_probe()
	await _vertex_metadata_probe()
	await _depth_and_blend_probe()
	await _camera_probe()
	await _fog_formula_probe()
	await _alpha_probe()
	await _sampler_probe()
	await _native_raster_probe()
	await _single_layer_probe()
	await _color_transfer_probe()
	results.failures = failures
	var file = FileAccess.open(output_dir.path_join("results.json"), FileAccess.WRITE)
	if file != null: file.store_string(JSON.stringify(results, "\t") + "\n")
	print("GPU3D_POC ", JSON.stringify(results))
	quit(0 if failures.is_empty() else 1)
