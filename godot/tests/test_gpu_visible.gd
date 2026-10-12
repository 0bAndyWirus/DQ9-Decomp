extends SceneTree
# Rendered, native regression of the real main.tscn/settings/start/frame path.
# Run with isolated XDG_DATA_HOME; ROM, save copies and captures stay local.
const Logic = preload("res://scripts/host_logic.gd")
var app
var inputs: Dictionary = {}
var failures: Array[String] = []
var captures: Dictionary = {}
var samples: Dictionary = {}
var out: String

func _initialize() -> void:
	call_deferred("_run")

func check(value: bool, message: String) -> void:
	if not value:
		failures.append(message)
		push_error(message)

func _argument(args: PackedStringArray, key: String, default: String) -> String:
	var index := args.find(key)
	return args[index + 1] if index >= 0 and index + 1 < args.size() else default

func _settle() -> void:
	for i in 4: await process_frame
	await RenderingServer.frame_post_draw

func _select(control: OptionButton, index: int) -> void:
	control.select(index)
	control.item_selected.emit(index)

func _compare_key() -> void:
	var event := InputEventKey.new()
	event.keycode = KEY_F6
	event.pressed = true
	root.push_input(event)
	event.pressed = false
	root.push_input(event)

func _sample_key(frame: int, checkpoints: PackedInt32Array) -> String:
	for checkpoint in checkpoints:
		if frame >= checkpoint - 119 and frame <= checkpoint: return str(checkpoint)
	return ""

func _sample_frame(key: String) -> void:
	var stats: Dictionary = app.machine.get_gpu3d_stats()
	if not samples.has(key): samples[key] = {"frames": 0, "valid": 0, "texture_frames": 0, "mask_frames": 0, "gpu_pixels": 0, "three_d_pixels": 0, "fallbacks": {}}
	var sample: Dictionary = samples[key]
	sample.frames += 1
	sample.valid += int(bool(stats.get("valid", false)))
	sample.texture_frames += int(app.machine.get_gpu3d_texture() != null)
	sample.mask_frames += int(app.machine.get_gpu3d_mask_texture() != null)
	sample.gpu_pixels += int(stats.get("gpu_pixels", 0))
	sample.three_d_pixels += int(stats.get("three_d_pixels", 0))
	var reason: String = stats.get("fallback", "disabled")
	sample.fallbacks[reason] = int(sample.fallbacks.get(reason, 0)) + 1

func _capture(frame: int) -> void:
	var machine: Node = app.machine
	var stats: Dictionary = machine.get_gpu3d_stats()
	check(bool(stats.get("valid", false)), "GPU-valid capture %d: %s" % [frame, stats])
	check(machine.get_gpu3d_texture() != null and machine.get_gpu3d_mask_texture() != null, "native texture and mask are non-null")
	if machine.get_gpu3d_texture() == null or machine.get_gpu3d_mask_texture() == null: return
	var mask: Image = machine.get_gpu3d_mask_texture().get_image()
	var mask_bytes := mask.get_data()
	var masked := 0
	for value in mask_bytes: masked += int(value != 0)
	check(masked > 100, "mask has visible GPU pixels")
	mask.save_png(out.path_join("%d_mask.png" % frame))
	var screen: TextureRect = app.top_rect if machine.get_gpu3d_screen_is_top() else app.bottom_rect
	var frame_stats := {"stats": stats, "mask_pixels": masked, "screen_is_top": machine.get_gpu3d_screen_is_top(), "windows": {}}
	var gpu_1x: Image
	# Diagnostic 1x render of the SAME frozen geometry, not a later guest frame.
	machine.set_gpu3d_scale(1)
	await _settle()
	gpu_1x = machine.get_gpu3d_texture().get_image()
	gpu_1x.save_png(out.path_join("%d_gpu_1x.png" % frame))
	for scale in [3, 6]:
		_select(app._ui.gpu_scale, scale - 2)
		await _settle()
		var gpu: Image = machine.get_gpu3d_texture().get_image()
		check(gpu.get_size() == Vector2i(256 * scale, 192 * scale), "hi-res buffer really has %dx samples" % scale)
		gpu.save_png(out.path_join("%d_gpu_%dx.png" % [frame, scale]))
		# Within-native-pixel variation rules out a nearest-upscaled 256x192 GPU buffer.
		var mixed_blocks := 0
		var difference_1x := 0
		for y in 192:
			for x in 256:
				if mask_bytes[y * 256 + x] == 0: continue
				var first := gpu.get_pixel(x * scale, y * scale)
				var mixed := false
				for dy in scale:
					for dx in scale:
						var pixel := gpu.get_pixel(x * scale + dx, y * scale + dy)
						mixed = mixed or pixel != first
						difference_1x += int(pixel != gpu_1x.get_pixel(x, y))
				mixed_blocks += int(mixed)
		check(mixed_blocks > 100 and difference_1x > 100, "hi-res masked area has new subpixel/edge samples at %dx" % scale)
		for window in [Vector2i(1280, 800), Vector2i(3072, 1920)]:
			root.size = window
			await _settle()
			check(is_instance_valid(app.gpu3d_rect) and app.gpu3d_rect.is_visible_in_tree(), "real app creates visible Gpu3D overlay")
			check(app.gpu3d_rect.get_parent() == screen, "overlay belongs to physical engine-A screen")
			check(app.gpu3d_rect.get_global_rect().is_equal_approx(screen.get_global_rect()), "mask/overlay align with the screen")
			var prefix := "%d_%dx_%dx%d" % [frame, scale, window.x, window.y]
			root.get_texture().get_image().save_png(out.path_join(prefix + "_status.png"))
			# A/B intentionally changes the host HUD text. Hide just statistics
			# for image measurement, not the screens, masks or compositor.
			app.status_label.hide()
			await _settle()
			var composite: Image = root.get_texture().get_image()
			composite.save_png(out.path_join(prefix + "_gpu.png"))
			# Actual F6 input selects software presentation without changing the
			# producer, guest frame, textures, renderer setting or saved config.
			var frozen: int = machine.get_stats().frame
			_compare_key()
			await _settle()
			check(app._gpu3d_compare_software and not app.gpu3d_rect.visible, "F6 selects software A/B")
			check(machine.get_stats().frame == frozen and machine.get_gpu3d_texture() != null, "paused F6 preserves the scene and exact guest frame")
			check(Logic.load_settings("user://settings.cfg").renderer_gpu3d == 1, "A/B is temporary, not a settings change")
			var software: Image = root.get_texture().get_image()
			software.save_png(out.path_join(prefix + "_software.png"))
			_compare_key()
			await _settle()
			check(not app._gpu3d_compare_software and app.gpu3d_rect.visible, "F6 restores GPU A/B")
			var changed := 0
			var outside := 0
			var sampled := 0
			var rect: Rect2 = screen.get_global_rect()
			rect.position = rect.position.round()
			for y in range(int(ceil(rect.position.y)), int(floor(rect.end.y)), 2):
				for x in range(int(ceil(rect.position.x)), int(floor(rect.end.x)), 2):
					var uv := (Vector2(x + 0.5, y + 0.5) - rect.position) / rect.size
					# Exact native-texel boundaries are float-rounding ties on
					# GL; test interiors rather than guessing the sampler's tie.
					var native := uv * Vector2(256, 192)
					if absf(native.x - roundf(native.x)) < 0.0001 or absf(native.y - roundf(native.y)) < 0.0001: continue
					var sx := clampi(int(uv.x * 256), 0, 255)
					var sy := clampi(int(uv.y * 192), 0, 191)
					var differs := composite.get_pixel(x, y) != software.get_pixel(x, y)
					if mask_bytes[sy * 256 + sx] != 0:
						changed += int(differs)
						sampled += 1
					else: outside += int(differs)
			check(changed > sampled / 200, "real composite visibly differs from software inside the mask")
			check(outside == 0, "mask-0/2D app pixels remain software-exact")
			frame_stats.windows[prefix] = {"screen_rect": [rect.position.x, rect.position.y, rect.size.x, rect.size.y], "masked_samples": sampled, "changed_samples": changed, "outside_samples_changed": outside, "mixed_native_blocks": mixed_blocks, "different_from_gpu_1x": difference_1x, "texture_filter": app.gpu3d_rect.texture_filter, "z_index": app.gpu3d_rect.z_index}
			check(app.status_label.text.contains("GPU3D: %dx" % scale) and app.status_label.text.contains("valid"), "GPU status reports real scale and validity")
			app.status_label.show()
			await _settle()
		# Filtering probe: the overlay's own nearest filter is independent of
		# the native 2D screen. Preserve the pixel-art default after measuring.
		app.gpu3d_rect.texture_filter = CanvasItem.TEXTURE_FILTER_LINEAR
		await _settle()
		root.get_texture().get_image().save_png(out.path_join("%d_%dx_linear.png" % [frame, scale]))
		app.gpu3d_rect.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
		# Diagnostic magenta tint proves WHERE the real composite shader draws.
		var material: ShaderMaterial = app.gpu3d_rect.material
		var shader := material.shader
		var tinted := Shader.new()
		tinted.code = shader.code.replace("COLOR = texture(TEXTURE, UV);", "COLOR = vec4(1.0, 0.0, 1.0, 1.0);")
		material.shader = tinted
		await _settle()
		root.get_texture().get_image().save_png(out.path_join("%d_%dx_tint.png" % [frame, scale]))
		material.shader = shader
		await _settle()
	captures[str(frame)] = frame_stats
	print("GPU_VISIBLE_CAPTURE ", frame, " ", JSON.stringify(frame_stats))

func _run() -> void:
	var args := OS.get_cmdline_user_args()
	out = _argument(args, "--app-output", "")
	var input_path := _argument(args, "--app-input", "")
	var rom := _argument(args, "--rom", "")
	var save := _argument(args, "--save", "")
	var checkpoint_list := _argument(args, "--app-checkpoints", "")
	if out.is_empty() or input_path.is_empty() or rom.is_empty() or save.is_empty() or checkpoint_list.is_empty():
		push_error("usage: Godot --path godot --script res://tests/test_gpu_visible.gd -- --rom <rom> --save <save-copy> --app-input <inputs> --app-output <dir> --app-checkpoints <frames> [--language en] [--app-rtc 0] [--app-timezone 0] [--app-start-scale 3] [--app-start-software]")
		quit(2)
		return
	DirAccess.make_dir_recursive_absolute(out)
	var initial := Logic.DEFAULTS.duplicate(true)
	initial.rom_path = rom
	initial.save_path = save
	initial.language = _argument(args, "--language", "en")
	initial.renderer_gpu3d = 0 if args.has("--app-start-software") else 1
	initial.gpu3d_scale = int(_argument(args, "--app-start-scale", "3"))
	Logic.save_settings("user://settings.cfg", initial)
	var file := FileAccess.open(input_path, FileAccess.READ)
	check(file != null, "input script exists")
	if file == null: quit(2); return
	while not file.eof_reached():
		var fields := file.get_line().strip_edges().split(" ", false)
		if fields.size() == 4: inputs[int(fields[0])] = [int(fields[1]), int(fields[2]), int(fields[3])]
	file.close()
	var checkpoints := PackedInt32Array()
	for value in checkpoint_list.split(","): checkpoints.append(int(value))
	checkpoints.sort()
	app = load("res://scenes/main.tscn").instantiate()
	root.add_child(app)
	await _settle()
	check(app.settings.renderer_gpu3d == initial.renderer_gpu3d and app.settings.gpu3d_scale == initial.gpu3d_scale, "real _ready loads persisted renderer and scale")
	check(app._ui.gpu_renderer.selected == initial.renderer_gpu3d and app._ui.gpu_scale.selected == initial.gpu3d_scale - 2, "settings panel reflects persisted selection")
	# Exercise the real Start button: machine construction, setting application,
	# load_game, start and the production frame_ready connection are not mocked.
	for button in app.find_children("*", "Button", true, false):
		if button.text == "Start" and button.custom_minimum_size.x == 180:
			button.pressed.emit()
			break
	check(app.machine != null and not app.settings_panel.visible, "actual Start creates native machine")
	if app.machine == null: quit(1); return
	app.machine.stop()
	# Release the just-started RTC session using load_game's documented error
	# path, then reload stopped with deterministic RTC, as host replay QA does.
	check(not app.machine.load_game("/nonexistent/gpu-visible-test.nds", save, Logic.firmware_language(initial.language)).is_empty(), "RTC reset unload rejects a missing ROM")
	app.machine.set_rtc_epoch(int(_argument(args, "--app-rtc", "0")), int(_argument(args, "--app-timezone", "0")))
	check(app.machine.load_game(rom, save, Logic.firmware_language(initial.language)).is_empty(), "deterministic reload of actual app machine")
	app.machine.error.connect(func(message: String): check(false, "native worker: " + message))
	var frame := 0
	while frame <= checkpoints[-1]:
		if initial.renderer_gpu3d == 0 and frame >= checkpoints[0] - 128 and app.settings.renderer_gpu3d == 0:
			_select(app._ui.gpu_renderer, 1)
		var state: Array = inputs.get(frame, [0, -1, -1])
		var count := 1
		while count < 32 and frame + count <= checkpoints[-1] and _sample_key(frame + count - 1, checkpoints).is_empty() and not (initial.renderer_gpu3d == 0 and frame + count == checkpoints[0] - 128) and inputs.get(frame + count, [0, -1, -1]) == state: count += 1
		app.machine.step_frames(count, int(state[0]), int(state[1]), int(state[2]), int(state[1]) >= 0 and int(state[2]) >= 0)
		await app.machine.frame_ready
		frame += count
		var sample_key := _sample_key(frame - 1, checkpoints)
		if not sample_key.is_empty(): _sample_frame(sample_key)
		if checkpoints.has(frame - 1):
			await _settle()
			await _capture(frame - 1)
	for key in samples:
		check(int(samples[key].frames) == 120, "all 120 real app frames sampled for " + key)
	# Settings changes at runtime must persist and disabling must not alter the
	# native software framebuffer, even though it releases the GPU resources.
	var before: PackedByteArray = app.machine.get_top_image().get_data()
	_select(app._ui.gpu_renderer, 0)
	await _settle()
	check(app.gpu3d_rect == null and app.machine.get_gpu3d_texture() == null, "runtime UI disable releases producer and overlay")
	check(app.machine.get_top_image().get_data() == before, "runtime disable leaves software image unchanged")
	check(Logic.load_settings("user://settings.cfg").renderer_gpu3d == 0, "software UI selection persists")
	_select(app._ui.gpu_renderer, 1)
	var warmup := {"producer_texture_null": app.machine.get_gpu3d_texture() == null, "overlay_exists": is_instance_valid(app.gpu3d_rect)}
	print("GPU_VISIBLE_WARMUP ", JSON.stringify(warmup))
	check(app.gpu3d_rect == null, "runtime re-enable stays software during producer warmup, not a deleted viewport")
	app.machine.step_frames(4, 0, -1, -1, false)
	await app.machine.frame_ready
	await _settle()
	check(is_instance_valid(app.gpu3d_rect) and app.machine.get_gpu3d_texture() != null, "runtime UI enable takes effect on real frames")
	var stored := Logic.load_settings("user://settings.cfg")
	check(stored.renderer_gpu3d == 1 and stored.gpu3d_scale == 6, "GPU UI selection and scale persist")
	var report := FileAccess.open(out.path_join("results.json"), FileAccess.WRITE)
	report.store_string(JSON.stringify({"captures": captures, "samples": samples, "warmup": warmup, "failures": failures}, "\t") + "\n")
	report.close()
	app._release_machine(false)
	app.free()
	print("GPU_VISIBLE_RESULT failures=", failures)
	quit(0 if failures.is_empty() else 1)
