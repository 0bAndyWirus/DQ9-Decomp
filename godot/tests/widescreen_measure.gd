extends SceneTree
# Render the real app in Off, Picture, or Full from the same scripted input.
# Run with isolated XDG_DATA_HOME and a disposable save copy; captures stay local.
const Logic = preload("res://scripts/host_logic.gd")

func _initialize() -> void:
	call_deferred("_run")

func _arg(args: PackedStringArray, key: String, fallback: String = "") -> String:
	var index := args.find(key)
	return args[index + 1] if index >= 0 and index + 1 < args.size() else fallback

func _fail(message: String) -> void:
	push_error(message)
	quit(2)

func _run() -> void:
	var args := OS.get_cmdline_user_args()
	var rom := _arg(args, "--rom")
	var save := _arg(args, "--save")
	var input_path := _arg(args, "--input")
	var out := _arg(args, "--output")
	var mode_name := _arg(args, "--mode")
	var modes := {"off": 0, "picture": 1, "full": 2}
	var frames := int(_arg(args, "--frames", "0"))
	var warmup := int(_arg(args, "--warmup", "120"))
	if rom.is_empty() or save.is_empty() or input_path.is_empty() or not out.is_absolute_path() or not modes.has(mode_name) or frames <= 0 or warmup < 0 or warmup >= frames:
		_fail("usage: --rom <rom> --save <disposable-save> --input <script> --output <absolute-dir> --mode off|picture|full --frames N [--warmup N] [--captures 1300,1999] [--language en] [--rtc 0] [--timezone 0]")
		return
	if DirAccess.make_dir_recursive_absolute(out) != OK:
		_fail("cannot create output directory: " + out)
		return
	var input := FileAccess.open(input_path, FileAccess.READ)
	if input == null:
		_fail("cannot open input: " + input_path)
		return
	var events := {}
	while not input.eof_reached():
		var line := input.get_line().strip_edges()
		if line.is_empty() or line.begins_with("#"): continue
		var fields := line.split(" ", false)
		if fields.size() != 4:
			_fail("invalid input row: " + line)
			return
		events[int(fields[0])] = [int(fields[1]), int(fields[2]), int(fields[3])]
	input.close()
	var captures := {}
	var capture_arg := _arg(args, "--captures")
	if not capture_arg.is_empty():
		for value in capture_arg.split(","):
			var frame := int(value)
			if frame < 0 or frame >= frames:
				_fail("capture frame outside replay: " + value)
				return
			captures[frame] = true
	var settings := Logic.DEFAULTS.duplicate(true)
	settings.rom_path = rom
	settings.save_path = save
	settings.language = _arg(args, "--language", "en")
	settings.renderer_gpu3d = 1
	settings.widescreen_mode = modes[mode_name]
	Logic.save_settings("user://settings.cfg", settings)
	var app = load("res://scenes/main.tscn").instantiate()
	root.add_child(app)
	await process_frame
	app._start_game()
	if app.machine == null:
		_fail("app could not start the game")
		return
	app.machine.stop()
	# Reset the launch-time RTC without replacing the app's configured mode.
	app.machine.load_game("/nonexistent/widescreen-measure.nds", save, Logic.firmware_language(settings.language))
	app.machine.set_rtc_epoch(int(_arg(args, "--rtc", "0")), int(_arg(args, "--timezone", "0")))
	var error: String = app.machine.load_game(rom, save, Logic.firmware_language(settings.language))
	if not error.is_empty():
		_fail("deterministic reload failed: " + error)
		return
	app.status_label.hide()
	var elapsed_us: Array[int] = []
	var heartbeat := FileAccess.open(out.path_join("heartbeat.log"), FileAccess.WRITE)
	if heartbeat == null:
		_fail("cannot write heartbeat")
		return
	for frame in frames:
		var state: Array = events.get(frame, [0, -1, -1])
		var start := Time.get_ticks_usec()
		app.machine.step_frames(1, int(state[0]), int(state[1]), int(state[2]), int(state[1]) >= 0 and int(state[2]) >= 0)
		await app.machine.frame_ready
		await process_frame
		await RenderingServer.frame_post_draw
		if frame % 500 == 0:
			var progress := "WIDESCREEN_FRAME %s %d/%d" % [mode_name, frame, frames]
			print(progress)
			heartbeat.store_line(progress)
			heartbeat.flush()
		if frame >= warmup: elapsed_us.append(Time.get_ticks_usec() - start)
		if captures.has(frame):
			var path := out.path_join("%s_%06d.png" % [mode_name, frame])
			if root.get_texture().get_image().save_png(path) != OK:
				_fail("cannot save capture: " + path)
				return
	elapsed_us.sort()
	var total := 0
	for value in elapsed_us: total += value
	var n := elapsed_us.size()
	var result := {"mode": mode_name, "frames": frames, "warmup": warmup, "samples": n,
		"mean_ms": float(total) / n / 1000.0, "median_ms": float(elapsed_us[(n - 1) / 2]) / 1000.0,
		"p95_ms": float(elapsed_us[int(ceil(n * 0.95)) - 1]) / 1000.0,
		"max_ms": float(elapsed_us[n - 1]) / 1000.0,
		"method": "one synchronous emulation step plus Godot presentation per sample; excludes screenshot encoding; not realtime pacing"}
	var report := FileAccess.open(out.path_join(mode_name + "_timing.json"), FileAccess.WRITE)
	if report == null:
		_fail("cannot write timing report")
		return
	report.store_string(JSON.stringify(result, "\t") + "\n")
	report.close()
	print("WIDESCREEN_MEASURE ", JSON.stringify(result))
	app._release_machine(false)
	app.free()
	quit(0)
