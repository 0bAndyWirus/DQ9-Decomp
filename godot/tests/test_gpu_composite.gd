extends SceneTree
# Requires a rendered backend: xvfb-run -a Godot --path godot --script res://tests/test_gpu_composite.gd
const Logic = preload("res://scripts/host_logic.gd")
const Stub = preload("res://scripts/gpu3d_stub.gd")
const TOP := Color8(31, 67, 151)
const BOTTOM := Color8(103, 43, 131)

class SyntheticHost extends "res://scripts/main.gd":
	func _ready() -> void:
		settings = Logic.DEFAULTS.duplicate(true)
		_build_ui()
		settings_panel.hide()
		status_label.hide()
	var persist_count := 0

	func _persist() -> void:
		persist_count += 1

var _pixels := 0
var _cases := 0
var _output := "res://../build/gpu-composite"

func _initialize() -> void:
	call_deferred("_run")

func _settle() -> void:
	for i in 3: await process_frame
	await RenderingServer.frame_post_draw

func _solid(color: Color) -> ImageTexture:
	var image := Image.create(256, 192, false, Image.FORMAT_RGBA8)
	image.fill(color)
	return ImageTexture.create_from_image(image)

func _check(image: Image, host: Control, gpu: bool, label: String) -> bool:
	var rects: Array[Rect2] = [host.top_rect.get_global_rect(), host.bottom_rect.get_global_rect()]
	# Godot's gui/snap_controls_to_pixels rounds the draw transform's origin;
	# get_global_rect() still reports the unsnapped layout position.
	for i in 2:
		rects[i].position = rects[i].position.round()
	for y in int(host.game_panel.size.y):
		for x in image.get_width():
			var point := Vector2(x + 0.5, y + 0.5)
			var screen := -1
			# Bottom is later in draw order, including merged-overlay overlap.
			for i in 2:
				var frame: Control = host.top_frame if i == 0 else host.bottom_frame
				if frame.visible and rects[i].has_point(point): screen = i
			if screen == -1: continue
			var uv := (point - rects[screen].position) / rects[screen].size
			var sx := clampi(int(floor(uv.x * 256)), 0, 255)
			var sy := clampi(int(floor(uv.y * 192)), 0, 191)
			var expected := TOP if screen == 0 else BOTTOM
			if gpu and (screen == 0) == host.gpu3d_screen_is_top and Stub.masked(sx, sy):
				expected = Stub.LEFT if sx < 128 else Stub.RIGHT
			var actual := image.get_pixel(x, y)
			if absf(actual.r - expected.r) > 1.1/255 or absf(actual.g - expected.g) > 1.1/255 or absf(actual.b - expected.b) > 1.1/255:
				push_error("%s at (%d,%d) screen=%d native=(%d,%d): got %s expected %s" % [label, x, y, screen, sx, sy, actual, expected])
				return false
			_pixels += 1
	return true

func _click(point: Vector2, pressed: bool) -> void:
	var event := InputEventMouseButton.new()
	event.position = point
	event.global_position = point
	event.button_index = MOUSE_BUTTON_LEFT
	event.pressed = pressed
	root.push_input(event)

func _key_probe(host: Control, image: Image, fraction: Vector2, expected_shape: int, expected_color: Color, label: String) -> bool:
	var rect: Rect2 = host._single_nodes[1].get_global_rect()
	rect.position = rect.position.round()
	var pixel := Vector2i(rect.position + rect.size * fraction)
	var point := Vector2(pixel) + Vector2(0.5, 0.5)
	var local: Vector2 = host.top_rect.get_global_transform().affine_inverse() * point
	var shape: int = host._single_shape_at(local)
	var color := image.get_pixelv(pixel)
	var matches := absf(color.r - expected_color.r) < 1.1 / 255.0 and absf(color.g - expected_color.g) < 1.1 / 255.0 and absf(color.b - expected_color.b) < 1.1 / 255.0
	if shape != expected_shape or not matches:
		push_error("%s: shape=%d color=%s, expected shape=%d color=%s" % [label, shape, color, expected_shape, expected_color])
		return false
	return true

func _run() -> void:
	root.size = Vector2i(1280, 800)
	var host := SyntheticHost.new()
	root.add_child(host)
	host.top_rect.texture = _solid(TOP)
	host.bottom_rect.texture = _solid(BOTTOM)
	var stub := Stub.new()
	root.add_child(stub)
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(_output))
	var ok := true
	ok = host._ui.gpu_renderer.item_count == 2 and host._ui.gpu_renderer.selected == 0 and ok
	ok = host._ui.gpu_scale.item_count == 5 and host._ui.gpu_scale.selected == 1 and host._ui.gpu_scale.disabled and ok
	# Software must not create an extra drawable, even when a producer publishes.
	stub.set_scale(3)
	stub.publish(host)
	await _settle()
	ok = ok and host.gpu3d_rect == null
	ok = _check(root.get_texture().get_image(), host, false, "software") and ok
	for scale in [2, 3, 6]:
		stub.set_scale(scale)
		host.settings.gpu3d_scale = scale
		host.settings.renderer_gpu3d = 1
		host._apply_gpu3d_settings()
		for layout in 7:
			host._set_layout(layout)
			for screen_is_top in [true, false]:
				stub.publish(host, screen_is_top)
				await _settle()
				var label := "scale%d_layout%d_%s" % [scale, layout, "top" if screen_is_top else "bottom"]
				var image := root.get_texture().get_image()
				image.save_png(_output.path_join(label + ".png"))
				ok = _check(image, host, true, label) and ok
				_cases += 1
	# Frame fallback removes the layer, instead of showing a stale GPU frame.
	host.set_gpu3d_frame(null, null)
	await _settle()
	ok = host.gpu3d_rect == null and ok
	ok = _check(root.get_texture().get_image(), host, false, "fallback") and ok
	# The single-screen shape is composited above the top GPU layer.
	host._set_layout(7)
	stub.publish(host, true)
	await _settle()
	var single_image := root.get_texture().get_image()
	var overlay: Rect2 = host._single_nodes[0].get_global_rect()
	var overlay_pixel := Vector2i(overlay.get_center())
	var actual := single_image.get_pixelv(overlay_pixel)
	ok = absf(actual.r - BOTTOM.r) < 1.1 / 255.0 and absf(actual.g - BOTTOM.g) < 1.1 / 255.0 and ok
	# A cropped source must not multiply by the unrelated texel at local UV.
	var patterned := Image.create(256, 192, false, Image.FORMAT_RGBA8)
	patterned.fill(TOP)
	patterned.fill_rect(Rect2i(128, 0, 128, 192), BOTTOM)
	host.bottom_rect.texture = ImageTexture.create_from_image(patterned)
	host.settings.single_screen_shapes[0].source = Rect2(128, 0, 128, 192)
	host._layout_screens()
	await _settle()
	var cropped_point := Vector2i(overlay.position + overlay.size * Vector2(0.2, 0.5))
	actual = root.get_texture().get_image().get_pixelv(cropped_point)
	ok = absf(actual.r - BOTTOM.r) < 1.1 / 255.0 and absf(actual.g - BOTTOM.g) < 1.1 / 255.0 and ok
	host.bottom_rect.texture = _solid(BOTTOM)
	host.settings.single_screen_shapes[0].source = Rect2(0, 0, 256, 192)
	host._layout_screens()
	var local: Vector2 = host.top_rect.get_global_transform().affine_inverse() * overlay.get_center()
	ok = host._single_shape_at(local) == 0 and ok
	host.settings.single_screen_shapes.append({"source":Rect2(0, 0, 256, 192), "destination":Rect2(0.65, 0.65, 0.35, 0.35), "color_key":BOTTOM})
	host._layout_screens()
	ok = host._single_shape_at(local) == 0 and ok
	await _settle()
	actual = root.get_texture().get_image().get_pixelv(overlay_pixel)
	ok = absf(actual.r - BOTTOM.r) < 1.1 / 255.0 and ok
	var adjacent := Image.create(256, 192, false, Image.FORMAT_RGBA8)
	adjacent.fill(BOTTOM)
	adjacent.set_pixel(1, 0, TOP)
	host.bottom_rect.texture = ImageTexture.create_from_image(adjacent)
	host.settings.single_screen_shapes[1].source = Rect2(0, 0, 2, 1)
	host._layout_screens()
	var first: Vector2 = host.top_rect.get_global_transform().affine_inverse() * (overlay.position + overlay.size * Vector2(0.49, 0.5))
	var second: Vector2 = host.top_rect.get_global_transform().affine_inverse() * (overlay.position + overlay.size * Vector2(0.51, 0.5))
	ok = host._single_shape_at(first) == 0 and host._single_shape_at(second) == 1 and ok
	# Two GPU subpixels within one DS pixel must make independent key decisions.
	# The lower shape supplies a distinct color wherever the upper shape keys out.
	var under := Color8(20, 190, 40)
	var source := Image.create(256, 192, false, Image.FORMAT_RGBA8)
	source.fill(under)
	source.set_pixel(0, 0, BOTTOM)
	source.set_pixel(1, 0, TOP)
	host.bottom_rect.texture = ImageTexture.create_from_image(source)
	host.settings.single_screen_shapes[0].source = Rect2(10, 10, 1, 1)
	host.settings.single_screen_shapes[1].source = Rect2(0, 0, 1, 1)
	host._layout_screens()
	var gpu := Image.create(512, 384, false, Image.FORMAT_RGBA8)
	gpu.fill(BOTTOM)
	gpu.set_pixel(1, 0, TOP)
	var mask := Image.create(256, 192, false, Image.FORMAT_RGBA8)
	mask.fill(Color.BLACK)
	mask.set_pixel(0, 0, Color.WHITE)
	host.set_gpu3d_frame(ImageTexture.create_from_image(gpu), ImageTexture.create_from_image(mask), false)
	await _settle()
	var keyed_image := root.get_texture().get_image()
	ok = _key_probe(host, keyed_image, Vector2(0.25, 0.25), 0, under, "GPU keyed subpixel") and ok
	ok = _key_probe(host, keyed_image, Vector2(0.75, 0.25), 1, TOP, "GPU visible subpixel") and ok
	# Transparent GPU pixels retain the software bottom before color-keying.
	gpu.fill(Color(1, 0, 0, 0))
	source.set_pixel(0, 0, TOP)
	host.bottom_rect.texture = ImageTexture.create_from_image(source)
	host._layout_screens()
	host.set_gpu3d_frame(ImageTexture.create_from_image(gpu), ImageTexture.create_from_image(mask), false)
	await _settle()
	keyed_image = root.get_texture().get_image()
	ok = _key_probe(host, keyed_image, Vector2(0.25, 0.25), 1, TOP, "transparent GPU retains software") and ok
	source.set_pixel(0, 0, BOTTOM)
	host.bottom_rect.texture = ImageTexture.create_from_image(source)
	host._layout_screens()
	await _settle()
	keyed_image = root.get_texture().get_image()
	ok = _key_probe(host, keyed_image, Vector2(0.25, 0.25), 0, under, "transparent GPU software key") and ok
	# An interior crop must not filter adjacent texels into any edge.
	host.set_gpu3d_frame(null, null)
	host.settings.nearest = false
	source.fill(under)
	source.fill_rect(Rect2i(10, 10, 4, 4), TOP)
	host.settings.single_screen_shapes[0].source = Rect2(20, 20, 1, 1)
	host.settings.single_screen_shapes[1].source = Rect2(10, 10, 4, 4)
	host.bottom_rect.texture = ImageTexture.create_from_image(source)
	host._layout_screens()
	host._apply_filter()
	await _settle()
	keyed_image = root.get_texture().get_image()
	ok = _key_probe(host, keyed_image, Vector2(0.98, 0.98), 1, TOP, "linear crop edge") and ok
	source.set_pixel(0, 0, BOTTOM)
	source.set_pixel(1, 0, TOP)
	host.settings.single_screen_shapes[0].source = Rect2(20, 20, 1, 1)
	host.bottom_rect.texture = ImageTexture.create_from_image(source)
	# Linear display filtering must not blend the key test across a DS pixel edge.
	host.set_gpu3d_frame(null, null)
	host.settings.nearest = false
	host.settings.single_screen_shapes[1].source = Rect2(0, 0, 2, 1)
	host._layout_screens()
	await _settle()
	keyed_image = root.get_texture().get_image()
	ok = _key_probe(host, keyed_image, Vector2(0.49, 0.5), 0, under, "linear keyed edge") and ok
	var edge_rect: Rect2 = host._single_nodes[1].get_global_rect()
	edge_rect.position = edge_rect.position.round()
	var edge_pixel := Vector2i(edge_rect.position + edge_rect.size * Vector2(0.51, 0.5))
	var edge_point := Vector2(edge_pixel) + Vector2(0.5, 0.5)
	var edge_local: Vector2 = host.top_rect.get_global_transform().affine_inverse() * edge_point
	var edge_color := keyed_image.get_pixelv(edge_pixel)
	ok = host._single_shape_at(edge_local) == 1 and absf(edge_color.g - under.g) > 0.1 and ok
	# Fractional crops must display the same nearest texel used for hit testing.
	host.settings.nearest = true
	host._apply_filter()
	# Keep both sampled texels visible so color-key masking cannot hide a mismatch.
	host.settings.single_screen_shapes[1].color_key = Color8(1, 2, 3)
	host.settings.single_screen_shapes[1].source = Rect2(0.75, 0, 1, 1)
	host._layout_screens()
	await _settle()
	keyed_image = root.get_texture().get_image()
	ok = _key_probe(host, keyed_image, Vector2(0.1, 0.5), 1, BOTTOM, "fractional nearest first texel") and ok
	ok = _key_probe(host, keyed_image, Vector2(0.9, 0.5), 1, TOP, "fractional nearest visible texel") and ok
	# A crop smaller than a pixel has one included texel and must not invert
	# linear filter clamp bounds or bleed into either neighboring texel.
	host.settings.nearest = false
	host._apply_filter()
	host.settings.single_screen_shapes[1].color_key = BOTTOM
	host.settings.single_screen_shapes[1].source = Rect2(1.1, 0.1, 0.2, 0.2)
	host._layout_screens()
	await _settle()
	keyed_image = root.get_texture().get_image()
	ok = _key_probe(host, keyed_image, Vector2(0.1, 0.5), 1, TOP, "subpixel linear crop start") and ok
	ok = _key_probe(host, keyed_image, Vector2(0.9, 0.5), 1, TOP, "subpixel linear crop end") and ok
	host.settings.nearest = true
	host._apply_filter()
	host.settings.nearest = true
	host.settings.single_screen_shapes[0].source = Rect2(0, 0, 256, 192)
	host.settings.single_screen_shapes[1].source = Rect2(0, 0, 2, 1)
	host.bottom_rect.texture = _solid(BOTTOM)
	host._layout_screens()
	stub.publish(host, false)
	await _settle()
	ok = bool((host._single_nodes[0].material as ShaderMaterial).get_shader_parameter("use_gpu")) and ok
	stub.publish(host, true)
	_click(overlay.get_center(), true)
	await process_frame
	ok = host._touching and ok
	_click(overlay.get_center(), false)
	await process_frame
	ok = not host._touching and ok
	host._set_layout(0)
	stub.publish(host, false)
	host.settings.renderer_gpu3d = 0
	host._apply_gpu3d_settings()
	await _settle()
	ok = host.gpu3d_rect == null and ok
	ok = _check(root.get_texture().get_image(), host, false, "software restored") and ok
	# Re-enable and verify the GPU child does not intercept touch routing.
	host.settings.renderer_gpu3d = 1
	host._apply_gpu3d_settings()
	await _settle()
	var center := host.bottom_rect.get_global_rect().get_center()
	_click(center, true)
	await process_frame
	ok = host._touching and ok
	_click(center, false)
	await process_frame
	ok = not host._touching and ok
	# Existing settings click routing remains intact with the new child present.
	for button in host.find_children("*", "Button", true, false):
		if button.text == "Start / Settings":
			_click(button.get_global_rect().get_center(), true)
			await process_frame
			_click(button.get_global_rect().get_center(), false)
			await process_frame
			ok = host.settings_panel.visible and ok
	host._ui.gpu_scale.select(4)
	host._ui.gpu_scale.item_selected.emit(4)
	ok = host.settings.gpu3d_scale == 6 and host.persist_count == 1 and ok
	host._ui.gpu_renderer.select(0)
	host._ui.gpu_renderer.item_selected.emit(0)
	ok = host.settings.renderer_gpu3d == 0 and host.gpu3d_rect == null and host._ui.gpu_scale.disabled and ok
	host._ui.gpu_renderer.select(1)
	host._ui.gpu_renderer.item_selected.emit(1)
	ok = host.settings.renderer_gpu3d == 1 and is_instance_valid(host.gpu3d_rect) and not host._ui.gpu_scale.disabled and host.persist_count == 3 and ok
	# Validity is a rolling frame ratio, not the current 3D-pixel share.
	host._clear_gpu3d_status()
	for i in 120:
		host._gpu3d_stats = {"valid": i < 112}
		host._observe_gpu3d_frame()
	host._gpu3d_stats = {"valid": true, "gpu_pixels": 93, "gpu_pixel_share": 0.93}
	ok = host._gpu3d_status_text().contains("valid 93%") and host._gpu3d_status_text().contains("3D pixels 93%") and ok
	# A small nonzero mask can round to 0%; it is still an active producer.
	host._gpu3d_stats = {"valid": true, "gpu_pixels": 1, "gpu_pixel_share": 0.00001}
	ok = host._gpu3d_status_text().contains("active") and ok
	host._gpu3d_stats = {"valid": false, "fallback": "w-buffer"}
	ok = host._gpu3d_status_text().contains("fallback: w-buffer") and ok
	print("%s: GPU composite %d layout/scale/swap cases, %d pixels, software/fallback/touch/settings" % ["PASS" if ok else "FAIL", _cases, _pixels])
	host.free()
	stub.free()
	quit(0 if ok else 1)
