extends SceneTree
# ROM-free; requires DQ9_GPU_SCENE_TESTS and a rendered backend (not --headless).
const Pixels = preload("res://tests/gpu3d_pixels.gd")

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	if not ClassDB.class_exists("DqMachine"):
		GDExtensionManager.load_extension("res://dq9.gdextension")
	if not ClassDB.class_exists("DqGpuSceneFixture"):
		push_error("configure -DDQ9_GPU_SCENE_TESTS=ON")
		quit(1)
		return
	var fixture: Node = ClassDB.instantiate("DqGpuSceneFixture")
	root.add_child(fixture)
	var stats: Dictionary = fixture.render_classic_overflow()
	var ok := int(stats.ram_polygon_count) == 2048 and int(stats.ram_vertex_count) == 2053
	ok = ok and bool(stats.ram_overflow) and int(stats.polygon_count) == 2048 and int(stats.vertex_count) == 2053
	ok = ok and bool(stats.polygon_overflow) and not bool(stats.vertex_overflow) and not bool(stats.enhanced_overflow)
	ok = ok and int(stats.opaque_count) == 2048 and int(stats.translucent_count) == 0 and bool(stats.capture_ready)
	if not ok or not bool(stats.get("scene_valid", false)):
		push_error("Classic overflow capture failed: " + str(stats))
		fixture.free()
		quit(1)
		return
	for i in 3: await process_frame
	await RenderingServer.frame_post_draw
	var scene: Node = fixture.get_scene()
	var gpu: Image = Pixels.straight_alpha(scene.get_texture().get_image())
	var reference: Image = Pixels.rgb555_display_image(scene.get_reference_image())
	var mask: PackedByteArray = scene.get_mask_texture().get_image().get_data()
	var covered := 0
	var bad := 0
	var missing := 0
	var error_sum := 0.0
	for y in 192:
		for x in 256:
			if mask[y * 256 + x] == 0: continue
			covered += 1
			var a := gpu.get_pixel(x, y)
			var b := reference.get_pixel(x, y)
			if a.a < 0.99: missing += 1
			var delta := Vector3(absf(a.r - b.r), absf(a.g - b.g), absf(a.b - b.b)) * 255.0
			error_sum += delta.x + delta.y + delta.z
			if maxf(delta.x, maxf(delta.y, delta.z)) > 8.0: bad += 1
	# The first red triangle is in front of the later green strip triangles.
	# Its interior verifies visible, nondegenerate geometry and depth ordering.
	var center := reference.get_pixel(128, 96)
	var mae := error_sum / maxi(1, covered * 3)
	var bad_percent := 100.0 * bad / maxi(1, covered)
	ok = covered > 100 and center.r > 0.8 and center.g < 0.1 and center.b < 0.1
	ok = ok and missing <= 1 and mae < 4.0 and bad_percent < 5.0
	print("CLASSIC_OVERFLOW stats=%s covered=%d missing=%d mae=%f bad_percent=%f center=%s" % [stats, covered, missing, mae, bad_percent, center])
	if not ok: push_error("Classic overflow retained geometry differs from same-render software")
	fixture.free()
	quit(0 if ok else 1)
