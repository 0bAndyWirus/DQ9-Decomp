extends SceneTree
## Real-emulator integration probe. Use a rendered backend (e.g. xvfb-run), not
## Godot --headless, because the SubViewport must produce an actual GPU image.

const Pixels = preload("res://tests/gpu3d_pixels.gd")
const Cracks = preload("res://tests/gpu3d_cracks.gd")
var failures: Array[String] = []
var fallback_counts: Dictionary = {}
var worker_errors: Array[String] = []
var input_by_frame: Dictionary = {}
var output_dir: String
var machine: Node
var comparisons: Dictionary = {}
var record_only := false
var ranges: Dictionary = {}

func _range_key(frame: int) -> String:
	for key in ranges:
		if frame >= int(ranges[key].first) and frame <= int(ranges[key].last):
			return key
	return ""

func _sample_range(frame: int, key: String) -> void:
	machine.set_gpu3d_scale(1)
	await _settle()
	var stats: Dictionary = machine.get_gpu3d_stats()
	var range_stats: Dictionary = ranges[key]
	range_stats.frames += 1
	var valid := bool(stats.get("valid", false))
	range_stats.valid += int(valid)
	var reason: String = stats.get("fallback", "disabled")
	range_stats.fallbacks[reason] = int(range_stats.fallbacks.get(reason, 0)) + 1
	range_stats.three_d_pixels += int(stats.get("three_d_pixels", 0))
	range_stats.gpu_pixels += int(stats.get("gpu_pixels", 0)) if valid else 0
	if not valid:
		return
	var scene := _scene()
	var mask: PackedByteArray = machine.get_gpu3d_mask_texture().get_image().get_data()
	var reference: Image = Pixels.rgb555_display_image(scene.get_reference_image())
	var metrics := _compare(Pixels.straight_alpha(machine.get_gpu3d_texture().get_image()), reference, mask, true)
	range_stats.measured += 1
	range_stats.mask_pixels += metrics.pixels
	range_stats.error_sum += metrics.mae_levels * metrics.pixels * 3
	range_stats.pixels_over_8 += metrics.pixels_over_8
	range_stats.max_mae = maxf(range_stats.max_mae, metrics.mae_levels)
	range_stats.max_percent_over_8 = maxf(range_stats.max_percent_over_8, metrics.percent_over_8)
	if metrics.mae_levels >= 4 or metrics.percent_over_8 >= 5:
		range_stats.quality_failures += 1
		if range_stats.quality_failure_frames.size() < 12:
			range_stats.quality_failure_frames.append(frame)
	range_stats.samples.append({"frame": frame, "mask_pixels": metrics.pixels,
		"mae_levels": metrics.mae_levels, "percent_over_8": metrics.percent_over_8,
		"gpu_pixel_share": stats.get("gpu_pixel_share", 0)})

func _compare(gpu: Image, reference: Image, mask: PackedByteArray, masked: bool) -> Dictionary:
	var a := gpu.get_data()
	var b := reference.get_data()
	var pixels := 0
	var missing := 0
	var extra := 0
	var bad := 0
	var error_sum := 0
	var alpha_error := 0
	for pixel in 256 * 192:
		var covered := b[pixel * 4 + 3] != 0
		if not covered and a[pixel * 4 + 3] != 0: extra += 1
		if not (mask[pixel] != 0 if masked else covered): continue
		pixels += 1
		if covered and a[pixel * 4 + 3] == 0: missing += 1
		var maximum := 0
		for channel in 3:
			var delta := absi(int(a[pixel * 4 + channel]) - int(b[pixel * 4 + channel]))
			error_sum += delta
			maximum = maxi(maximum, delta)
		alpha_error += absi(int(a[pixel * 4 + 3]) - int(b[pixel * 4 + 3]))
		if maximum > 8: bad += 1
	return {"pixels": pixels, "mae_levels": float(error_sum) / maxi(1, pixels * 3),
		"percent_over_8": 100.0 * bad / maxi(1, pixels), "pixels_over_8": bad,
		"missing_alpha": missing, "extra_alpha": extra, "alpha_mae_levels": float(alpha_error) / maxi(1, pixels)}

func _initialize() -> void:
	call_deferred("_run")

func check(condition: bool, message: String) -> void:
	if not condition:
		failures.append(message)
		push_error(message)

func _scene() -> Node:
	return machine.get_node_or_null("Gpu3DScene")

func _settle() -> void:
	for i in 3:
		await process_frame
	await RenderingServer.frame_post_draw

func _mesh_vertices(scene: Node) -> Array:
	var result: Array = []
	for child in scene.get_node("Gpu3DViewport").get_children():
		if child is MeshInstance3D and child.mesh:
			for surface in child.mesh.get_surface_count():
				result.append(child.mesh.surface_get_arrays(surface)[Mesh.ARRAY_VERTEX])
	return result

func _paused_widescreen_switch() -> void:
	var scene := _scene()
	await _settle()
	var frame: int = machine.get_gpu3d_stats().frame
	var off := _mesh_vertices(scene)
	var off_size: Vector2 = machine.get_gpu3d_texture().get_size()
	machine.set_gpu3d_widescreen_enabled(true)
	await _settle()
	check(_scene() == scene, "paused Picture retains the live scene")
	var picture := _mesh_vertices(scene)
	var picture_size: Vector2 = machine.get_gpu3d_texture().get_size()
	check(not off.is_empty() and picture != off, "paused Picture rebuilds retained frame geometry")
	check(picture_size.x > off_size.x and picture_size.y == off_size.y, "paused Picture widens viewport")
	check(int(machine.get_gpu3d_stats().frame) == frame, "paused Picture keeps the same guest frame")
	# A mode change bumps the worker epoch; refreshing the retained frame must
	# still rebuild it while paused, including for HD texture setting changes.
	for child in scene.get_node("Gpu3DViewport").get_children():
		if child is MeshInstance3D and child.mesh:
			child.mesh.clear_surfaces()
	machine.set_hd_textures_enabled(not machine.get_hd_textures_enabled())
	check(_mesh_vertices(scene) == picture, "paused HD toggle refreshes the retained frame after a mode change")
	machine.set_hd_textures_enabled(not machine.get_hd_textures_enabled())
	machine.set_gpu3d_widescreen_enabled(false)
	await _settle()
	check(_scene() == scene, "paused Off retains the live scene")
	check(_mesh_vertices(scene).is_empty() and machine.get_gpu3d_texture() == null,
		"paused Off releases Picture geometry and falls back to software")
	check(int(machine.get_gpu3d_stats().frame) == frame, "paused Off keeps the same guest frame")
	machine.set_gpu3d_widescreen_enabled(true)
	machine.set_gpu3d_widescreen_enabled(false)
	await _settle()
	check(_scene() == scene and _mesh_vertices(scene).is_empty() and machine.get_gpu3d_texture() == null,
		"rapid paused toggles do not restore released Picture geometry")

func _polygon_coverage(scene: Node) -> Dictionary:
	var effects: SubViewport = scene.get_node("Gpu3DEffects")
	var post: MeshInstance3D = effects.get_node("Gpu3DPost")
	post.visible = false
	await _settle()
	var metadata := effects.get_texture().get_image()
	var textured_materials: Dictionary = {}
	for child in effects.get_children():
		if not child is MeshInstance3D or child == post: continue
		for surface in child.mesh.get_surface_count():
			var material: ShaderMaterial = child.get_surface_override_material(surface)
			if bool(material.get_shader_parameter("textured")):
				textured_materials[material.get_instance_id()] = material
				material.set_shader_parameter("textured", false)
	await _settle()
	var geometry := effects.get_texture().get_image()
	for material in textured_materials.values():
		material.set_shader_parameter("textured", true)
	post.visible = true
	await _settle()
	return {"metadata": metadata, "geometry": geometry}

func _alignment_fixture() -> void:
	check(ClassDB.class_exists("DqGpuSceneFixture"), "configure -DDQ9_GPU_SCENE_TESTS=ON for temporal alignment regression")
	if not ClassDB.class_exists("DqGpuSceneFixture"):
		return
	var fixture: Node = ClassDB.instantiate("DqGpuSceneFixture")
	root.add_child(fixture)
	for frame in 6:
		var stats: Dictionary = fixture.advance()
		var scene: Node = fixture.get_scene()
		if frame == 0:
			check(not bool(stats.valid) and scene.get_texture() == null, "first frame has no previous 3D snapshot")
			continue
		check(int(stats.source_frame) + 1 == int(stats.frame), "displayed GPU scene is from prior scanline214")
		await _settle()
		var image: Image = scene.get_texture().get_image()
		var gpu := image.get_data()
		var software_mask: PackedByteArray = stats.software_mask
		var mismatched := 0
		for pixel in 256 * 192:
			if (gpu[pixel * 4 + 3] != 0) != (software_mask[pixel] != 0):
				mismatched += 1
		check(mismatched == 0, "alternating-GX frame %d has %d silhouette mismatches" % [frame, mismatched])
		print("GPU_SCENE_ALIGNMENT frame=%d source=%d silhouette_mismatch=%d" % [stats.frame, stats.source_frame, mismatched])
	fixture.free()

func _crack_fixture() -> void:
	var fixture: Node = ClassDB.instantiate("DqGpuSceneFixture")
	root.add_child(fixture)
	var measurements: Dictionary = {}
	var initial_pixels := PackedByteArray()
	for scale in [1, 3, 6]:
		fixture.render_probe({"shared_edges": true, "color": 0x7c00, "clear_alpha": 31, "scale": scale})
		await _settle()
		var scene: Node = fixture.get_scene()
		var gpu := Pixels.straight_alpha(scene.get_texture().get_image())
		var reference: Image = Pixels.rgb555_display_image(scene.get_reference_image())
		var mask: PackedByteArray = scene.get_mask_texture().get_image().get_data()
		if scale == 1:
			initial_pixels = gpu.get_data()
			measurements["1x"] = _compare(gpu, reference, mask, true)
			check(measurements["1x"].mae_levels < 4 and measurements["1x"].percent_over_8 < 5,
				"shared-edge native GX fixture retains scale1 quality")
		else:
			var coverage := await _polygon_coverage(scene)
			var metrics := Cracks.measure(gpu, reference, mask, scene.get_reference_polygon_ids(), coverage.metadata, coverage.geometry, scale)
			var highlight: Image = metrics.highlight
			check(highlight.save_png(output_dir.path_join("fixture_%dx_cracks.png" % scale)) == OK, "save shared-edge fixture cracks")
			metrics.erase("highlight")
			measurements["%dx" % scale] = metrics
			if not record_only:
				check(metrics.crack_pixels == 0, "shared-edge GX fixture is watertight at scale%d: %s" % [scale, metrics])
		check(gpu.save_png(output_dir.path_join("fixture_%dx_gpu.png" % scale)) == OK, "save shared-edge GX fixture")
	fixture.get_scene().set_scale(1)
	await _settle()
	check(Pixels.straight_alpha(fixture.get_scene().get_texture().get_image()).get_data() == initial_pixels,
		"switching hi-res back to scale1 resets cached conservative-overlap uniforms")
	comparisons["_crack_fixture"] = measurements
	print("GPU_SCENE_CRACK_FIXTURE " + JSON.stringify(measurements))
	fixture.free()

func _capture(frame: int, scale: int) -> void:
	machine.set_gpu3d_scale(scale)
	await _settle()
	var stats: Dictionary = machine.get_gpu3d_stats()
	check(bool(stats.get("color_valid", false)), "captured color pass is renderable: " + str(stats))
	check(int(stats.get("surface_count", 0)) > 0, "captured scene has material surfaces")
	check(int(stats.get("polygon_count", -1)) == int(stats.get("captured_polygon_count", -2)), "owned sorted polygon count equals RenderFrame's captured count")
	check(int(stats.get("polygon_count", 0)) <= 2048 and int(stats.get("vertex_count", 0)) <= 6144, "bounded geometry copy")
	check(int(stats.get("textures_cached", 0)) > 0, "scene textures were decoded and cached")
	check(int(stats.get("decoded_this_frame", 257)) <= 256, "per-frame decode budget")
	var scene := _scene()
	if scene == null or not bool(stats.get("color_valid", false)):
		return
	var surfaces := 0
	for child in scene.get_node("Gpu3DViewport").get_children():
		if not child is MeshInstance3D or not child.name.begins_with("Gpu3DPolygons"): continue
		surfaces += child.mesh.get_surface_count()
		for surface in child.mesh.get_surface_count():
			var arrays: Array = child.mesh.surface_get_arrays(surface)
			check(arrays[Mesh.ARRAY_VERTEX].size() == arrays[Mesh.ARRAY_TEX_UV2].size(), "every clip vertex carries W in UV2")
	check(surfaces == int(stats.surface_count), "reported surfaces belong to all ArrayMesh chunks")
	var texture: Texture2D = machine.get_gpu3d_texture() if bool(stats.valid) else scene.get_raw_texture()
	var mask_texture: Texture2D = scene.get_raw_mask_texture()
	check(texture != null and mask_texture != null, "raw viewport and mask resources exist")
	if texture == null or mask_texture == null:
		return
	var gpu := Pixels.straight_alpha(texture.get_image())
	var mask: Image = mask_texture.get_image()
	check(gpu.get_size() == Vector2i(256 * scale, 192 * scale), "GPU scale changes viewport resolution")
	check(mask.get_size() == Vector2i(256, 192) and mask.get_format() == Image.FORMAT_R8, "mask stays native-resolution R8")
	var bytes := mask.get_data()
	var zeros := 0
	var ones := 0
	for value in bytes:
		if value == 0: zeros += 1
		elif value == 1: ones += 1
		else: check(false, "mask contains a value other than 0/1")
	check(ones == int(stats.gpu_pixels), "published hybrid mask agrees with worker pixel counts")
	var gpu_pixels := gpu.get_data()
	var covered := 0
	for pixel in gpu.get_width() * gpu.get_height():
		if gpu_pixels[pixel * 4 + 3] != 0: covered += 1
	check(covered > 0, "SubViewport actually rendered the character")
	if not bool(stats.valid):
		check(machine.get_gpu3d_texture() == null and machine.get_gpu3d_mask_texture() == null, "unsupported effects cannot leak an incomplete compositor overlay")
	check(scene.has_method("get_reference_image"), "test build exposes same-render-start software 3D")
	if not scene.has_method("get_reference_image"): return
	var reference: Image = scene.get_reference_image()
	check(reference != null, "owned software raster accompanies the geometry")
	if reference == null: return
	var display_reference := Pixels.rgb555_display_image(reference)
	check(int(stats.source_frame) + 1 == int(stats.frame), "GPU and reference use the delayed render epoch")
	if scale == 1:
		var metrics := {"mask": _compare(gpu, display_reference, bytes, true),
			"coverage": _compare(gpu, display_reference, bytes, false),
			"valid": bool(stats.valid), "fallback": stats.fallback, "source_frame": stats.source_frame}
		metrics["three_d_pixels"] = stats.three_d_pixels
		metrics["gpu_pixel_share"] = stats.gpu_pixel_share
		if stats.has("reference_disp3dcnt"):
			metrics["registers"] = {"disp3dcnt": stats.reference_disp3dcnt,
				"clear_color": stats.reference_clear_color, "fog_color": stats.reference_fog_color}
		if stats.has("captured_snapshots"):
			metrics["captures"] = {"total": stats.captured_snapshots, "valid": stats.valid_snapshots}
			metrics["alpha_classes"] = {"native_translucent": stats.native_translucent_polygons,
				"blending": stats.blending_polygons}
		metrics["capture_causes"] = {}
		for cause in ["partial_polygon_pixels", "partial_texel_pixels", "shadow_pixels", "wireframe_pixels", "opaque_decal_pixels"]:
			metrics.capture_causes[cause] = stats.get(cause, 0)
		metrics["partial_top_polygons"] = stats.get("partial_top_polygons", [])
		if bool(stats.effects_active):
			metrics["pre_effects_native_rgb6_mask"] = _compare(Pixels.straight_alpha(scene.get_raw_texture().get_image()), reference, bytes, true)
		comparisons[str(frame)] = metrics
		print("GPU_SCENE_PARITY frame=%d %s" % [frame, JSON.stringify(metrics)])
		if bool(stats.valid) and not record_only:
			check(metrics.mask.mae_levels < 4 and metrics.mask.percent_over_8 < 5,
				"same-snapshot mask parity at %d: %s" % [frame, metrics])
	if scale == 3 and bool(stats.valid):
		var bounds := Rect2i()
		for y in range(192):
			for x in range(256):
				if reference.get_pixel(x, y).a > 0:
					bounds = Rect2i(x, y, 1, 1) if not bounds.has_area() else bounds.merge(Rect2i(x, y, 1, 1))
		var allowed := Rect2i(bounds.position * 3 - Vector2i(3, 3), bounds.size * 3 + Vector2i(6, 6))
		var escaped := 0
		for y in range(gpu.get_height()):
			for x in range(gpu.get_width()):
				if gpu.get_pixel(x, y).a > 0 and not allowed.has_point(Vector2i(x, y)): escaped += 1
		check(escaped == 0, "3x raw geometry has no terminal-edge spikes: %d pixels" % escaped)
		comparisons[str(frame)]["hires_escaped_pixels"] = escaped
	if scale > 1 and bool(stats.valid):
		check(scene.has_method("get_reference_polygon_ids"), "same-snapshot polygon IDs accompany RGB6/A5")
		if scene.has_method("get_reference_polygon_ids"):
			var native_ids: PackedByteArray = scene.get_reference_polygon_ids()
			var coverage := await _polygon_coverage(scene)
			var crack_metrics := Cracks.measure(gpu, display_reference, bytes, native_ids, coverage.metadata, coverage.geometry, scale)
			var highlight: Image = crack_metrics.highlight
			crack_metrics.erase("highlight")
			comparisons[str(frame)]["cracks_%dx" % scale] = crack_metrics
			check(highlight.save_png(output_dir.path_join("%04d_%dx_cracks.png" % [frame, scale])) == OK, "save crack footprint")
			check(coverage.metadata.save_png(output_dir.path_join("%04d_%dx_ids.png" % [frame, scale])) == OK, "save GPU polygon IDs")
			check(coverage.geometry.save_png(output_dir.path_join("%04d_%dx_geometry_ids.png" % [frame, scale])) == OK, "save texture-independent GPU coverage IDs")
			var native_id_image := Image.create_from_data(256, 192, false, Image.FORMAT_R8, native_ids)
			check(native_id_image.save_png(output_dir.path_join("%04d_%dx_reference_ids.png" % [frame, scale])) == OK, "save native polygon IDs")
			print("GPU_SCENE_CRACKS frame=%d scale=%d %s" % [frame, scale, JSON.stringify(crack_metrics)])
			if not record_only:
				check(crack_metrics.crack_pixels == 0, "no interior polygon cracks at frame%d scale%d: %s" % [frame, scale, crack_metrics])
	var software: Image = machine.get_top_image() if machine.get_gpu3d_screen_is_top() else machine.get_bottom_image()
	var prefix := "%04d_%dx" % [frame, scale]
	check(gpu.save_png(output_dir.path_join(prefix + "_gpu.png")) == OK, "save GPU PNG")
	check(software.save_png(output_dir.path_join(prefix + "_software.png")) == OK, "save software PNG")
	check(reference.save_png(output_dir.path_join(prefix + "_reference3d.png")) == OK, "save same-snapshot software 3D PNG")
	check(display_reference.save_png(output_dir.path_join(prefix + "_reference_display.png")) == OK, "save same-snapshot RGB555 display reference")
	check(mask.save_png(output_dir.path_join(prefix + "_mask.png")) == OK, "save mask PNG")
	var composite := software.duplicate() as Image
	composite.resize(256 * scale, 192 * scale, Image.INTERPOLATE_NEAREST)
	for y in 192 * scale:
		for x in 256 * scale:
			if bytes[(y / scale) * 256 + x / scale] != 0:
				composite.set_pixel(x, y, gpu.get_pixel(x, y))
	check(composite.save_png(output_dir.path_join(prefix + "_raw_composite.png")) == OK, "save diagnostic color-pass composite")
	print("GPU_SCENE_CAPTURE frame=%d scale=%d mask0=%d mask1=%d covered=%d stats=%s" % [frame, scale, zeros, ones, covered, stats])

func _run() -> void:
	var args := OS.get_cmdline_user_args()
	record_only = args.has("--record-only")
	if record_only: args.remove_at(args.find("--record-only"))
	for argument in args.duplicate():
		if not argument.begins_with("--ranges="): continue
		args.remove_at(args.find(argument))
		for value in argument.trim_prefix("--ranges=").split(","):
			var endpoints := value.split("-")
			if endpoints.size() != 2:
				push_error("invalid sample range: " + value)
				quit(2)
				return
			ranges[value] = {"first": int(endpoints[0]), "last": int(endpoints[1]),
				"frames": 0, "valid": 0, "measured": 0, "three_d_pixels": 0, "gpu_pixels": 0,
				"mask_pixels": 0, "error_sum": 0.0, "pixels_over_8": 0,
				"max_mae": 0.0, "max_percent_over_8": 0.0, "quality_failures": 0,
				"quality_failure_frames": [], "fallbacks": {}, "samples": []}
	if args.size() < 6 or args.size() > 9:
		push_error("usage: Godot --path godot --script res://tests/test_gpu_scene.gd -- <rom> <save-copy> <input-script> <output-dir> <frames> <checkpoints> [language=1] [rtc=0] [timezone=0] [--ranges=first-last,...] [--record-only]")
		quit(2)
		return
	var frames := int(args[4])
	var checkpoints := PackedInt32Array()
	for value in args[5].split(","):
		checkpoints.append(int(value))
	checkpoints.sort()
	for frame in checkpoints:
		if frame < 1 or frame >= frames:
			push_error("capture checkpoint is outside replay: %d" % frame)
			quit(2)
			return
	for key in ranges:
		check(int(ranges[key].first) >= 1 and int(ranges[key].last) < frames and
			int(ranges[key].first) <= int(ranges[key].last), "sample range lies within replay: " + key)
	output_dir = args[3]
	check(DirAccess.make_dir_recursive_absolute(output_dir) == OK, "create screenshot directory")
	var input_file := FileAccess.open(args[2], FileAccess.READ)
	if input_file == null:
		push_error("cannot open input script: " + args[2])
		quit(2)
		return
	while not input_file.eof_reached():
		var parts := input_file.get_line().strip_edges().split(" ", false)
		if parts.size() == 4:
			input_by_frame[int(parts[0])] = [int(parts[1]), int(parts[2]), int(parts[3])]
	input_file.close()
	if not ClassDB.class_exists("DqMachine"):
		GDExtensionManager.load_extension("res://dq9.gdextension")
	machine = ClassDB.instantiate("DqMachine")
	check(machine != null and ClassDB.class_exists("DqGpuScene"), "both extension classes are registered")
	if machine == null:
		quit(3)
		return
	root.add_child(machine)
	machine.error.connect(func(message: String): worker_errors.append(message))
	machine.set_rtc_epoch(int(args[7]) if args.size() >= 8 else 0, int(args[8]) if args.size() >= 9 else 0)
	check(machine.get_gpu3d_texture() == null and _scene() == null, "disabled default allocates no GPU scene")
	machine.set_gpu3d_enabled(true)
	machine.set_gpu3d_scale(1)
	var error: String = machine.load_game(args[0], args[1], int(args[6]) if args.size() >= 7 else 1)
	if not error.is_empty():
		push_error(error)
		machine.free()
		quit(4)
		return
	await _alignment_fixture()
	check(Cracks.self_test(), "crack metric detects missing-polygon stripes but not high-resolution texels")
	await _crack_fixture()
	machine.set_paused(true)
	var frame := 0
	var paused_switch_checked := false
	while frame < frames:
		var state: Array = input_by_frame.get(frame, [0, -1, -1])
		var count := 1
		# Batch only equal per-frame input, never cross a capture boundary.
		while count < 32 and frame + count < frames and not checkpoints.has(frame + count - 1) and _range_key(frame + count - 1).is_empty() and input_by_frame.get(frame + count, [0, -1, -1]) == state:
			count += 1
		machine.step_frames(count, int(state[0]), int(state[1]), int(state[2]), int(state[1]) >= 0 and int(state[2]) >= 0)
		await machine.frame_ready
		frame += count
		var stats: Dictionary = machine.get_gpu3d_stats()
		var reason: String = stats.get("fallback", "disabled")
		fallback_counts[reason] = int(fallback_counts.get(reason, 0)) + 1
		check(int(stats.get("decoded_this_frame", 0)) <= 256, "texture budget at frame %d" % (frame - 1))
		var range_key := _range_key(frame - 1)
		if not range_key.is_empty(): await _sample_range(frame - 1, range_key)
		if checkpoints.has(frame - 1):
			await _capture(frame - 1, 1)
			await _capture(frame - 1, 3)
			await _capture(frame - 1, 6)
		# The probe invalidates the retained GPU frame, so run it only after
		# all measurements for this publication have finished.
		if not paused_switch_checked and bool(stats.get("valid", false)) and int(stats.get("mesh_polygon_count", 0)) > 0:
			await _paused_widescreen_switch()
			paused_switch_checked = true
	check(paused_switch_checked, "paused widescreen test encountered a valid nonempty GPU frame")
	# Publish under the old epoch, then switch modes before _process can
	# consume it. The new software frame must not retain the previous scene.
	var stale_scene := _scene()
	var cached_before: int = machine.get_gpu3d_stats().get("textures_cached", 0)
	machine.step_frames(2, 0, -1, -1, false)
	machine.set_gpu3d_widescreen_enabled(true)
	await machine.frame_ready
	check(_scene() == stale_scene and machine.get_gpu3d_texture() == null
		and machine.get_gpu3d_raw_texture() == null
		and not bool(machine.get_gpu3d_stats().get("valid", false)),
		"old-epoch publication invalidates the previous scene")
	check(int(machine.get_gpu3d_stats().get("textures_cached", 0)) == cached_before,
		"rejecting an old-epoch publication retains the texture cache")
	machine.set_gpu3d_widescreen_enabled(false)
	check(machine.get_gpu3d_texture() == null and machine.get_gpu3d_raw_texture() == null,
		"paused mode toggle does not redraw the rejected publication")
	machine.step_frames(2, 0, -1, -1, false)
	await machine.frame_ready
	# Dropped ring publications must not lose new texture payloads. Synchronous
	# multi-frame stepping replaces several unconsumed slots before one upload.
	machine.step_frames(8, 0, -1, -1, false)
	await machine.frame_ready
	check(int(machine.get_gpu3d_stats().get("textures_cached", 0)) > 0, "texture cache survives replaced frame slots")
	var software_before: PackedByteArray = machine.get_top_image().get_data()
	var released_scene: WeakRef = weakref(_scene())
	machine.set_gpu3d_enabled(false)
	check(_scene() == null and machine.get_gpu3d_texture() == null and machine.get_gpu3d_mask_texture() == null, "disable releases Godot GPU scene and overlay resources")
	check(released_scene.get_ref() == null, "disable destroys the old scene immediately while paused")
	check(machine.get_top_image().get_data() == software_before, "toggle leaves software pixels unchanged")
	machine.set_gpu3d_enabled(true)
	check(_scene() == null and machine.get_gpu3d_texture() == null,
		"reenabling while paused does not restore a stale scene")
	await process_frame
	check(_scene() == null and machine.get_gpu3d_texture() == null,
		"paused processing does not recreate the scene without a new frame")
	machine.step_frames(2, 0, -1, -1, false)
	await machine.frame_ready
	check(int(machine.get_gpu3d_stats().get("textures_cached", 0)) > 0, "reenable epoch rebuilds texture cache instead of trusting stale acknowledgements")
	check(worker_errors.is_empty(), "no emulator errors: " + str(worker_errors))
	print("GPU_SCENE_FALLBACKS consumed_frames=%d reasons=%s" % [frames, fallback_counts])
	for key in ranges:
		var measured: Dictionary = ranges[key]
		measured["gpu_valid_percent"] = 100.0 * measured.valid / maxi(1, measured.frames)
		measured["gpu_pixel_share"] = float(measured.gpu_pixels) / maxi(1, measured.three_d_pixels)
		measured["mae_levels"] = measured.error_sum / maxi(1, measured.mask_pixels * 3)
		measured["percent_over_8"] = 100.0 * measured.pixels_over_8 / maxi(1, measured.mask_pixels)
		var summary := measured.duplicate()
		summary.erase("samples")
		print("GPU_SCENE_RANGE %s %s" % [key, JSON.stringify(summary)])
		check(measured.frames == measured.last - measured.first + 1, "every frame in range is sampled: " + key)
		if not record_only:
			check(measured.gpu_valid_percent >= 80, ">=80% GPU-valid frames in range " + key)
			check(measured.quality_failures == 0, "every displayed mask meets MAE<4 and <5% over8 in range " + key)
	comparisons["_ranges"] = ranges
	var report := FileAccess.open(output_dir.path_join("metrics.json"), FileAccess.WRITE)
	if report != null:
		report.store_string(JSON.stringify(comparisons, "\t") + "\n")
		report.close()
	# Leave the frame_ready emission before freeing its emitter.
	await process_frame
	machine.stop()
	check(machine.flush_save(), "save flush succeeds before scene teardown")
	machine.free()
	await process_frame
	OS.delay_msec(500)
	if not failures.is_empty():
		print("FAIL: GPU scene integration (%d assertions)" % failures.size())
		quit(1)
		return
	print("PASS: real geometry/texture/mask capture, scales1/3/6, crack metric, skipped-publication cache, toggle, clean shutdown")
	quit(0)
