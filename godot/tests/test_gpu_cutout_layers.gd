extends SceneTree
# ROM-free; requires DQ9_GPU_SCENE_TESTS and a rendered backend.
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
	var stats: Dictionary = fixture.render_probe({"cutout_layers": true, "scale": 3})
	for i in 3: await process_frame
	await RenderingServer.frame_post_draw
	var scene: Node = fixture.get_scene()
	var gpu: Image = Pixels.straight_alpha(scene.get_texture().get_image())
	var reference: Image = Pixels.rgb555_display_image(scene.get_reference_image())
	var mask: PackedByteArray = scene.get_mask_texture().get_image().get_data()
	var covered := 0
	var control_errors := 0
	var holes := 0
	var unsafe := 0
	# This control region is an untextured opaque foreground, well inside edges.
	for y in range(70, 120):
		for x in range(68, 84):
			if mask[y * 256 + x] == 0: continue
			covered += 1
			var a := gpu.get_pixel(x * 3 + 1, y * 3 + 1)
			var b := reference.get_pixel(x, y)
			if maxf(absf(a.r - b.r), maxf(absf(a.g - b.g), absf(a.b - b.b))) > 0.1:
				control_errors += 1
	# Two alternating texels per native pixel create visible holes in 3x.
	# The generated mask must not select any native pixel containing such a hole.
	for y in range(70, 120):
		for x in range(100, 156):
			var lo := 1.0
			var hi := 0.0
			for sx in 3:
				var c := gpu.get_pixel(x * 3 + sx, y * 3 + 1)
				if c.g < lo:
					lo = c.g
				hi = maxf(hi, c.g)
			if hi - lo > 0.25:
				holes += 1
				if mask[y * 256 + x] != 0: unsafe += 1
	# The GPU's partial pass need not correctly compose two layers: that is
	# precisely why the cutout holes must be masked out. Remove only the
	# foreground and check the native raster to prove both layers contribute.
	var layer_stats: Dictionary = fixture.render_probe({"cutout_layers": true, "cutout_no_foreground": true, "scale": 3})
	var layer_reference: Image = Pixels.rgb555_display_image(scene.get_reference_image())
	var layer_color := layer_reference.get_pixel(128, 96)
	var layers_visible := layer_color.r > 0.1 and layer_color.b > 0.1
	var ok := int(stats.polygon_count) == 5 and int(layer_stats.polygon_count) == 4 and covered > 100 and control_errors == 0 and holes > 100 and layers_visible and unsafe == 0
	print("CUTOUT_LAYERS polygons=%d covered=%d control_errors=%d holes=%d layers=%s unsafe=%d" % [stats.polygon_count, covered, control_errors, holes, layer_color, unsafe])
	if not ok: push_error("scale-3 cutout multilayer mask regression")
	fixture.free()
	quit(0 if ok else 1)
