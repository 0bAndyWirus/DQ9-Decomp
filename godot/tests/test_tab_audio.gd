extends SceneTree

# Exercises the actual launcher and its Tab handler, not just set_speed().
var app: Control
var machine: Node
var phase := 0
var began := 0
var previous_ms := 0
var previous_frame := 0
var failed := false
var max_main_ms := 0.0
var process_sum_ms := 0.0
var process_samples := 0
var finish_frame := 3000

func _initialize() -> void: call_deferred("_run")

func _run() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() < 2 or args.size() > 4:
		push_error("test_tab_audio.gd -- <rom> <fresh-save> [return-speed=1] [end-frame=3000]")
		quit(2)
		return
	app = load("res://scenes/main.tscn").instantiate()
	root.add_child(app)
	app.settings.rom_path = args[0]
	app.settings.save_path = args[1]
	app.settings.language = "en"
	app.settings.backup_type = "eeprom64k"
	app._start_game()
	machine = app.machine
	if machine == null:
		push_error("Native game failed to start")
		quit(3)
		return
	if args.size() >= 3: app._user_speed = float(args[2])
	if args.size() >= 4: finish_frame = maxi(2100, int(args[3]))
	began = Time.get_ticks_msec()
	previous_ms = began

func _key(pressed: bool) -> void:
	var event := InputEventKey.new()
	event.keycode = KEY_TAB
	event.physical_keycode = KEY_TAB
	event.pressed = pressed
	Input.parse_input_event(event)

func _process(_delta: float) -> bool:
	if machine == null: return false
	var stats: Dictionary = machine.get_stats()
	var frame := int(stats.frame)
	var now := Time.get_ticks_msec()
	if now - began > 2000:
		var main_ms := Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0
		max_main_ms = maxf(max_main_ms, main_ms)
		process_sum_ms += main_ms
		process_samples += 1
	if frame >= 1000 and phase == 0:
		_key(true)
		phase = 1
	if phase == 1 and app._ff_held:
		print("TAB_HELD ", stats)
		phase = 2
	if frame >= 2000 and phase == 2:
		_key(false)
		print("TAB_RELEASE_BEFORE ", stats)
		phase = 3
	if phase == 3 and not app._ff_held:
		print("TAB_RELEASE_AFTER ", stats)
		# This UI sample is one host frame after the trim, so paced production
		# may already have refilled it (especially at 2x). audio_pass.gd checks
		# the immediate 1x queue bound; the native ring test checks exact retention.
		if float(stats.fast_forward_discarded_ms) <= 0.0:
			push_error("Tab release did not discard its stale source backlog")
			failed = true
		phase = 4
	if now - previous_ms >= 1000:
		print("HOST second=", (now-began)/1000.0, " frame=", frame, " fps=", (frame-previous_frame)*1000.0/(now-previous_ms), " stats=", stats)
		previous_ms = now
		previous_frame = frame
	if frame >= finish_frame:
		var ok: bool = phase == 4 and not failed
		print("TAB_AUDIO ", "PASS" if ok else "FAIL", " elapsed_ms=", now-began, " main_avg_ms=", process_sum_ms/max(1,process_samples), " main_max_ms=", max_main_ms, " stats=", stats)
		_key(false)
		app._release_machine(false)
		machine = null
		app.free()
		call_deferred("_finish", ok)
	return false

func _finish(ok: bool) -> void:
	await create_timer(2).timeout
	quit(0 if ok else 1)
