extends SceneTree
# ROM-free real GX fixture; requires DQ9_GPU_SCENE_TESTS and a rendered backend.

func _initialize() -> void:
	call_deferred("_run")

func _settle() -> void:
	for i in 3: await process_frame
	await RenderingServer.frame_post_draw

func _width(image: Image) -> int:
	var left := image.get_width()
	var right := -1
	for y in image.get_height():
		for x in image.get_width():
			if image.get_pixel(x, y).a > 0.5:
				left = mini(left, x)
				right = maxi(right, x)
	return right - left + 1 if right >= left else 0

func _run() -> void:
	if not ClassDB.class_exists("DqMachine"):
		GDExtensionManager.load_extension("res://dq9.gdextension")
	if not ClassDB.class_exists("DqGpuSceneFixture"):
		push_error("configure -DDQ9_GPU_SCENE_TESTS=ON")
		quit(1)
		return
	var fixture: Node = ClassDB.instantiate("DqGpuSceneFixture")
	root.add_child(fixture)
	fixture.render_probe({"quad": true, "scale": 3})
	var scene: Node = fixture.get_scene()
	scene.set_widescreen_enabled(true)
	scene.set_guest_widescreen(false)
	fixture.render_probe({"quad": true, "scale": 3})
	await _settle()
	var picture := _width(scene.get_texture().get_image())
	scene.set_guest_widescreen(true)
	fixture.render_probe({"quad": true, "scale": 3})
	await _settle()
	var full := _width(scene.get_texture().get_image())
	# Identical guest clip vertices: Picture applies 3/4 to clip X, Full must
	# not apply it again after the guest projection already widened the FOV.
	var ok := picture > 0 and full > 0 and absf(float(picture) / float(full) - 0.75) < 0.03
	if not ok: push_error("FOV-once widths: Picture=%d Full=%d" % [picture, full])
	if not scene.get_guest_widescreen():
		push_error("guest widescreen state was not retained")
		ok = false
	scene.set_widescreen_enabled(false)
	fixture.render_probe({"quad": true, "scale": 3})
	await _settle()
	var off_guest := _width(scene.get_texture().get_image())
	scene.set_guest_widescreen(false)
	fixture.render_probe({"quad": true, "scale": 3})
	await _settle()
	var off_native := _width(scene.get_texture().get_image())
	if scene.get_guest_widescreen() or off_native <= 0 or off_guest != off_native:
		push_error("Off must ignore guest widescreen: guest=%d native=%d" % [off_guest, off_native])
		ok = false
	scene.set_widescreen_enabled(true)
	fixture.render_probe({"quad": true, "scale": 3})
	await _settle()
	var picture_again := _width(scene.get_texture().get_image())
	if picture_again != picture:
		push_error("Picture width after toggling: %d != %d" % [picture_again, picture])
		ok = false
	scene.set_guest_widescreen(true)
	# Clear still participates in 3D rendering; unoccupied side pixels become
	# neutral black while widened geometry and Off clear remain intact.
	fixture.render_probe({"quad": true, "wide_quad": true, "clear_alpha": 31, "scale": 3})
	await _settle()
	var wide: Image = scene.get_texture().get_image()
	var clear_side: Color = wide.get_pixel(int(wide.get_width() * 0.98), int(wide.get_height() * 0.05))
	var geometry_side: Color = wide.get_pixel(int(wide.get_width() * 0.89), int(wide.get_height() * 0.5))
	if clear_side.r > 0.01 or clear_side.g > 0.01 or clear_side.b > 0.01 or geometry_side.r + geometry_side.g + geometry_side.b < 0.05:
		push_error("wide clear/geometry: %s / %s" % [clear_side, geometry_side])
		ok = false
	# One GPU texel at the native top/left boundary must not leak clear color.
	var top_seam: Color = wide.get_pixel(wide.get_width() / 2, 0)
	var left_seam: Color = wide.get_pixel((wide.get_width() - 256 * 3) / 2, 8)
	for seam in [top_seam, left_seam]:
		if seam.r > 0.01 or seam.g > 0.01 or seam.b > 0.01:
			push_error("wide native-edge clear seam: %s" % seam)
			ok = false
	# Picture, opaque and partial clear: test the final exposed texel of the
	# geometry-phase strip across rounded wide viewport sizes.
	scene.set_guest_widescreen(false)
	for scale in [1, 4, 5, 6]:
		for clear_alpha in [16, 31]:
			fixture.render_probe({"quad": true, "clear_alpha": clear_alpha, "scale": scale})
			await _settle()
			var probe: Image = scene.get_texture().get_image()
			var side: Color = probe.get_pixel(0, int(probe.get_height() * 0.05))
			if side.r > 0.01 or side.g > 0.01 or side.b > 0.01:
				push_error("Picture backdrop scale=%d alpha=%d: %s" % [scale, clear_alpha, side])
				ok = false
			var phase_row := int(floor((0.5 - 1.0 / 32.0) * scale - 0.5))
			if phase_row >= 0:
				var edge: Color = probe.get_pixel(probe.get_width() / 2, phase_row)
				if edge.r > 0.01 or edge.g > 0.01 or edge.b > 0.01:
					push_error("phase seam scale=%d alpha=%d: %s" % [scale, clear_alpha, edge])
					ok = false
	scene.set_guest_widescreen(true)
	for scale in [4, 5, 6]:
		fixture.render_probe({"quad": true, "wide_quad": true, "clear_alpha": 31, "scale": scale})
		await _settle()
		var border_probe: Image = scene.get_texture().get_image()
		var native_left: float = (float(border_probe.get_width()) - 256.0 * scale) * 0.5
		var ratio: float = float(border_probe.get_width()) / (256.0 * scale)
		var last_phase_x := int(floor(native_left + (0.5 - 1.0 / 32.0) * ratio * scale - 0.5))
		var left_clear: Color = border_probe.get_pixel(last_phase_x, scale * 8)
		var right_clear: Color = border_probe.get_pixel(int(ceil(native_left + 256.0 * scale)), scale * 8)
		for clear_pixel in [left_clear, right_clear]:
			if clear_pixel.r + clear_pixel.g + clear_pixel.b > 0.03:
				push_error("native boundary clear scale=%d: %s" % [scale, clear_pixel])
				ok = false
		for x in [int(ceil(native_left)), int(floor(native_left + 256.0 * scale - 1.0))]:
			var geometry: Color = border_probe.get_pixel(x, border_probe.get_height() / 2)
			if geometry.r + geometry.g + geometry.b < 0.05:
				push_error("native boundary geometry scale=%d x=%d: %s" % [scale, x, geometry])
				ok = false
	fixture.render_probe({"quad": true, "wide_quad": true, "clear_alpha": 31, "scale": 3})
	await _settle()
	# Resizing a retained, paused frame must update both shader boundaries
	# without requiring a new guest frame.
	var post_mesh := scene.get_node("Gpu3DEffects/Gpu3DPost") as MeshInstance3D
	var clear_mesh := scene.get_node("Gpu3DViewport/Gpu3DPolygons") as MeshInstance3D
	var post_material := post_mesh.mesh.surface_get_material(0) as ShaderMaterial
	var clear_material := clear_mesh.mesh.surface_get_material(0) as ShaderMaterial
	for scale in [1, 2]:
		scene.set_scale(scale)
		var ratio := float((256 * scale * 4 + 1) / 3) / float(256 * scale)
		if absf(float(post_material.get_shader_parameter("native_width_ratio")) - ratio) > 0.00001 or absf(float(clear_material.get_shader_parameter("side_native_width_ratio")) - ratio) > 0.00001:
			push_error("paused scale %d has stale wide boundary" % scale)
			ok = false
	fixture.render_probe({"quad": true, "wide_quad": true, "clear_alpha": 31,
		"alpha": 16, "blend": true, "fog": true, "polygon_fog": true,
		"clear_fog": true, "edges": true, "scale": 3})
	await _settle()
	var partial: Image = scene.get_texture().get_image()
	var partial_clear: Color = partial.get_pixel(int(partial.get_width() * 0.98), int(partial.get_height() * 0.05))
	var partial_geometry: Color = partial.get_pixel(int(partial.get_width() * 0.89), int(partial.get_height() * 0.5))
	if partial_clear.r > 0.01 or partial_clear.g > 0.01 or partial_clear.b > 0.01 or partial_geometry.r + partial_geometry.g + partial_geometry.b < 0.05:
		push_error("wide partial/fog/edge clear and geometry: %s / %s" % [partial_clear, partial_geometry])
		ok = false
	fixture.render_probe({"quad": true, "wide_quad": true, "clear_alpha": 31,
		"clear_depth": 0, "scale": 3})
	await _settle()
	var occluded: Image = scene.get_texture().get_image()
	var hidden: Color = occluded.get_pixel(int(occluded.get_width() * 0.89), int(occluded.get_height() * 0.5))
	if hidden.r > 0.01 or hidden.g > 0.01 or hidden.b > 0.01:
		push_error("geometry behind clear depth must not mark coverage: %s" % hidden)
		ok = false
	scene.set_widescreen_enabled(false)
	fixture.render_probe({"quad": true, "wide_quad": true, "clear_alpha": 31, "scale": 3})
	await _settle()
	var off: Image = scene.get_texture().get_image()
	if off.get_pixel(0, 0).r + off.get_pixel(0, 0).g + off.get_pixel(0, 0).b < 0.05:
		push_error("Off must retain clear colour")
		ok = false
	fixture.free()
	print("PASS: FOV-once" if ok else "FAIL: FOV-once")
	quit(0 if ok else 1)
