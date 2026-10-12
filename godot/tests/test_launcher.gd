extends SceneTree

const ACTIONS := ["ds_a", "ds_b", "ds_select", "ds_start", "ds_right", "ds_left", "ds_up", "ds_down", "ds_r", "ds_l", "ds_x", "ds_y"]

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	if not ClassDB.class_exists("DqMachine"):
		_fail("Native DqMachine class is unavailable")
		return
	var inputs := {}
	var args := OS.get_cmdline_user_args()
	for i in args.size():
		if args[i] == "--input-script" and i + 1 < args.size():
			var file := FileAccess.open(args[i + 1], FileAccess.READ)
			if file == null:
				_fail("Cannot open scripted input")
				return
			while not file.eof_reached():
				var parts := file.get_line().strip_edges().split(" ", false)
				if parts.size() == 4:
					inputs[int(parts[0])] = [int(parts[1]), int(parts[2]), int(parts[3])]
			file.close()
	var app = load("res://scenes/main.tscn").instantiate()
	root.add_child(app)
	await process_frame
	if app.machine == null or app.machine.get_class() != "DqMachine" or app.settings_panel.visible:
		_fail("Actual scene did not autostart the native machine")
		return
	var started := Time.get_ticks_msec()
	var next_sample := 0
	var initial_frame := int(app.machine.get_stats().frame)
	var top_hashes := {}
	var bottom_hashes := {}
	var samples := []
	while Time.get_ticks_msec() - started < 60000:
		await process_frame
		var stats: Dictionary = app.machine.get_stats()
		var frame := int(stats.frame)
		var state: Array = inputs.get(frame, [0, -1, -1])
		for bit in ACTIONS.size():
			if int(state[0]) & (1 << bit): Input.action_press(ACTIONS[bit])
			else: Input.action_release(ACTIONS[bit])
		app.machine.set_touch(maxi(0, state[1]), maxi(0, state[2]), state[1] >= 0 and state[2] >= 0)
		var elapsed := Time.get_ticks_msec() - started
		if elapsed >= next_sample:
			# process_frame precedes node processing, so allow the label to describe
			# the most recently presented frame rather than the worker's next frame.
			var label: String = app.status_label.text
			var displayed := int(label.get_slice(" ", 1))
			if displayed < initial_frame or displayed > frame:
				_fail("UI frame label is invalid: " + label)
				return
			var top: Image = app.machine.get_top_image()
			var bottom: Image = app.machine.get_bottom_image()
			if top == null or bottom == null or top.is_empty() or bottom.is_empty():
				_fail("Native launcher screens are empty")
				return
			top_hashes[_digest(top)] = true
			bottom_hashes[_digest(bottom)] = true
			samples.append(displayed)
			next_sample += 10000
	var final_stats: Dictionary = app.machine.get_stats()
	if int(final_stats.frame) <= initial_frame or samples.size() < 6 or int(samples.back()) <= int(samples.front()):
		_fail("Native/UI frame counter did not progress")
		return
	if top_hashes.size() < 2 or bottom_hashes.size() < 2:
		_fail("Native screen images did not change")
		return
	print("LAUNCHER_RESULT elapsed_ms=%d initial_frame=%d final_frame=%d emulation_fps=%.2f top_hashes=%d bottom_hashes=%d stats=%s" % [Time.get_ticks_msec()-started, initial_frame, final_stats.frame, float(final_stats.frame-initial_frame)/60.0, top_hashes.size(), bottom_hashes.size(), final_stats])
	for action in ACTIONS: Input.action_release(action)
	app._stop_game()
	if app.machine != null:
		_fail("Production close-game path retained a machine")
		return
	await process_frame
	app.queue_free()
	await process_frame
	print("Native launcher acceptance passed (60 seconds, scripted observation-based input, save flushed)")
	quit(0)

func _digest(image: Image) -> String:
	var hash := HashingContext.new()
	hash.start(HashingContext.HASH_SHA256)
	hash.update(image.get_data())
	return hash.finish().hex_encode()

func _fail(message: String) -> void:
	push_error(message)
	quit(1)
