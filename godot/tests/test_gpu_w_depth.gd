extends SceneTree
# ROM-free; requires DQ9_GPU_SCENE_TESTS and a rendered backend (not --headless).
# xvfb-run -a Godot --path godot --script res://tests/test_gpu_w_depth.gd
const Pixels = preload("res://tests/gpu3d_pixels.gd")
var failures: Array[String] = []

func _initialize() -> void:
	call_deferred("_run")

func check(condition: bool, message: String) -> void:
	if not condition:
		failures.append(message)
		push_error(message)

func _settle() -> void:
	for i in 3: await process_frame
	await RenderingServer.frame_post_draw

func _materials(viewport: Node) -> Dictionary:
	var result := {}
	for child in viewport.get_children():
		if not child is MeshInstance3D or child.name == "Gpu3DPost" or not child.mesh: continue
		for surface in child.mesh.get_surface_count():
			var material: ShaderMaterial = child.get_surface_override_material(surface)
			if material == null: material = child.mesh.surface_get_material(surface)
			if material: result[material.get_instance_id()] = material
	return result

func _run() -> void:
	if not ClassDB.class_exists("DqMachine"):
		GDExtensionManager.load_extension("res://dq9.gdextension")
	if not ClassDB.class_exists("DqGpuSceneFixture"):
		push_error("configure -DDQ9_GPU_SCENE_TESTS=ON")
		quit(1)
		return
	var fixture: Node = ClassDB.instantiate("DqGpuSceneFixture")
	root.add_child(fixture)
	var previous_color := {}
	var previous_metadata := {}
	for w in [false, true, false, true]:
		var stats: Dictionary = fixture.render_probe({"depth_order": true, "w_buffer": w, "reuse_generation": true})
		await _settle()
		var scene: Node = fixture.get_scene()
		check(int(stats.polygon_count) == 2 and bool(stats.valid), "depth fixture renders two polygons")
		var reference: Image = Pixels.rgb555_display_image(scene.get_reference_image())
		var gpu: Image = Pixels.straight_alpha(scene.get_texture().get_image())
		var expected := reference.get_pixel(128, 96)
		var actual := gpu.get_pixel(128, 96)
		check((expected.r > 0.8 and expected.g < 0.1) if w else (expected.g > 0.8 and expected.r < 0.1),
			"software raster chooses the %s winner: %s" % ["W" if w else "Z", expected])
		check(absf(actual.r - expected.r) < 0.08 and absf(actual.g - expected.g) < 0.08,
			"GPU matches software depth winner in %s mode: %s vs %s" % ["W" if w else "Z", actual, expected])
		var mask: PackedByteArray = scene.get_raw_mask_texture().get_image().get_data()
		var gpu_bytes := gpu.get_data()
		var reference_bytes := reference.get_data()
		var eligible := 0
		var bad := 0
		var total_error := 0
		for p in 256 * 192:
			if mask[p] == 0: continue
			eligible += 1
			var worst := 0
			for c in 3:
				var delta := absi(int(gpu_bytes[p * 4 + c]) - int(reference_bytes[p * 4 + c]))
				total_error += delta
				worst = maxi(worst, delta)
			if worst > 8: bad += 1
		var mae := float(total_error) / maxi(1, eligible * 3)
		print("GPU_W_DEPTH_PIXELS mode=", "W" if w else "Z", " eligible=", eligible, " bad=", bad, " mae=", mae)
		check(eligible > 0 and mae < 4.0 and float(bad) / maxi(1, eligible) < 0.05,
			"same-snapshot depth fixture stays within GPU pixel tolerance")
		var color_materials := _materials(scene.get_node("Gpu3DViewport"))
		var effects: SubViewport = scene.get_node("Gpu3DEffects")
		var metadata_materials := _materials(effects)
		check(not color_materials.is_empty() and not metadata_materials.is_empty(), "color and metadata materials exist")
		for material in color_materials.values() + metadata_materials.values():
			check(bool(material.get_shader_parameter("w_buffer")) == w, "cached polygon material follows depth mode")
		if not previous_color.is_empty():
			var reused_color := false
			var reused_metadata := false
			for id in color_materials: reused_color = reused_color or previous_color.has(id)
			for id in metadata_materials: reused_metadata = reused_metadata or previous_metadata.has(id)
			check(reused_color and reused_metadata, "mode toggles reuse color and metadata materials")
		previous_color = color_materials
		previous_metadata = metadata_materials
		var post: MeshInstance3D = effects.get_node("Gpu3DPost")
		var post_material: ShaderMaterial = post.mesh.surface_get_material(0)
		check(bool(post_material.get_shader_parameter("w_buffer")) == w, "postprocess depth decoding follows depth mode")
		post.visible = false
		await _settle()
		var metadata := effects.get_texture().get_image().get_pixel(128, 96)
		# IDs are encoded as 64 + 3*id in the red channel.
		var expected_id := 1 if w else 2
		check(absf(metadata.r * 255.0 - (64 + expected_id * 3)) < 2.0,
			"metadata depth winner ID %d: %s" % [expected_id, metadata])
		post.visible = true
		await _settle()
		print("GPU_W_DEPTH mode=%s gpu=%s software=%s metadata=%s" % ["W" if w else "Z", actual, expected, metadata])
	# A translucent green layer is in front only in Z mode; W depth rejects it.
	for w in [false, true]:
		fixture.render_probe({"depth_order": true, "depth_partial": true, "blend": true, "w_buffer": w, "reuse_generation": true})
		await _settle()
		var scene: Node = fixture.get_scene()
		var reference: Image = Pixels.rgb555_display_image(scene.get_reference_image())
		var gpu: Image = Pixels.straight_alpha(scene.get_raw_texture().get_image())
		var expected := reference.get_pixel(128, 96)
		var actual := gpu.get_pixel(128, 96)
		check(expected.r > 0.35 and (expected.g < 0.1 if w else expected.g > 0.35),
			"software partial-layer depth result: %s" % expected)
		check(absf(actual.r - expected.r) < 0.15 and absf(actual.g - expected.g) < 0.15,
			"GPU partial-layer depth matches software: %s vs %s" % [actual, expected])
	fixture.free()
	print("GPU_W_DEPTH_RESULT failures=", failures)
	quit(0 if failures.is_empty() else 1)
