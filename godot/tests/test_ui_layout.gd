extends SceneTree
# Regression: in a real window the game screens must be laid out and the pause panel hidden at start.
# Run under a display: xvfb-run -a Godot --path godot --script res://tests/test_ui_layout.gd
func _init() -> void:
	var scene: Control = load("res://scenes/main.tscn").instantiate()
	root.add_child(scene)
	for i in 10: await process_frame
	var ok := true
	if scene.pause_panel.visible: push_error("pause panel visible at start"); ok = false
	for frame in [scene.top_frame, scene.bottom_frame]:
		if not frame.visible or frame.size.x < 100 or frame.size.y < 100:
			push_error("screen frame not laid out: %s" % [frame.size]); ok = false
	# Horizontal layout reserves both exposed sides of the engine-A screen.
	scene.settings.renderer_gpu3d = 1
	scene._active_widescreen_mode = 1
	var wide := Image.create(384, 192, false, Image.FORMAT_RGBA8)
	wide.fill(Color.BLACK)
	wide.fill_rect(Rect2i(120, 0, 16, 192), Color.RED)
	var mask := Image.create(256, 192, false, Image.FORMAT_RGBA8)
	mask.fill(Color.WHITE)
	var wide_texture := ImageTexture.create_from_image(wide)
	var mask_texture := ImageTexture.create_from_image(mask)
	scene._set_layout(1)
	for top in [true, false]:
		scene.set_gpu3d_frame(wide_texture, mask_texture, top)
		var a: Rect2 = scene.top_frame.get_rect()
		var b: Rect2 = scene.bottom_frame.get_rect()
		var side: float = a.size.x * 0.25
		if (a.end.x + side if top else a.end.x) > (b.position.x if top else b.position.x - side):
			push_error("wide horizontal screens overlap"); ok = false
		if (a.position.x - side if top else a.position.x) < -0.01 or (b.end.x if top else b.end.x + side) > scene.game_panel.size.x + 0.01:
			push_error("wide horizontal screens exceed panel"); ok = false
	# Both the rendered crop and color-key hit test must use native mask UVs
	# but sample the central native region of the wider GPU image.
	scene.settings_panel.hide()
	scene.status_label.hide()
	var base := Image.create(256, 192, false, Image.FORMAT_RGBA8)
	base.fill(Color.BLACK)
	scene.bottom_rect.texture = ImageTexture.create_from_image(base)
	base.fill(Color.BLUE)
	scene.top_rect.texture = ImageTexture.create_from_image(base)
	scene.settings.single_screen_shapes = [{"source": Rect2(0, 0, 256, 192), "destination": Rect2(0, 0, 1, 1), "opacity": 1.0, "color_key": Color.BLACK}]
	scene._set_layout(7)
	for mode in [1, 2]:
		scene._active_widescreen_mode = mode
		scene.set_gpu3d_frame(wide_texture, mask_texture, false)
		for i in 3: await process_frame
		await RenderingServer.frame_post_draw
		var rect: Rect2 = scene._single_nodes[0].get_global_rect()
		var point := rect.position.round() + rect.size * Vector2(0.25, 0.5)
		var local: Vector2 = scene.top_rect.get_global_transform().affine_inverse() * point
		var pixel := root.get_texture().get_image().get_pixelv(Vector2i(point))
		if scene._single_shape_at(local) != 0 or pixel.r < 0.9 or pixel.b > 0.1:
			push_error("wide single-screen crop or color-key hit test misaligned in mode %d: %s" % [mode, pixel]); ok = false
	print("PASS: ui layout" if ok else "FAIL: ui layout")
	quit(0 if ok else 1)
