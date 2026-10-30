extends SceneTree
# Invoked by tools/attract_regression.py with isolated user data and save copies.
const Logic = preload("res://scripts/host_logic.gd")
const FRAMES := 600
var events := {"live": 0, "frames": 0, "released": 0}

func _initialize() -> void:
	call_deferred("_run")

func arg(args: PackedStringArray, name: String) -> String:
	var i := args.find(name)
	return args[i + 1] if i >= 0 and i + 1 < args.size() else ""

func fail(message: String) -> void:
	push_error("FAIL: " + message)
	quit(1)

func wait_live(attract: Node, prior: int) -> bool:
	var deadline := Time.get_ticks_msec() + 120000
	while events.live <= prior and Time.get_ticks_msec() < deadline and attract.active:
		await process_frame
	return events.live > prior and events.frames > 0

func replay(app: Node, rom: String, save: String, language: String, epoch: int, timezone: int, inputs: PackedStringArray, output: String) -> bool:
	app.machine.stop()
	app.machine.load_game("/nonexistent/checkpoint-test.nds", save, Logic.firmware_language(language))
	if DirAccess.copy_absolute(arg(OS.get_cmdline_user_args(), "--source-save"), save) != OK:
		push_error("Cannot restore checkpoint save")
		return false
	app.machine.set_rtc_epoch(epoch, timezone)
	var error: String = app.machine.load_game(rom, save, Logic.firmware_language(language))
	if not error.is_empty():
		push_error("Replay load: " + error)
		return false
	error = app.machine.set_input_script(inputs)
	if not error.is_empty():
		push_error("Replay input: " + error)
		return false
	var finished := [-1]
	var on_finished := func(frame): finished[0] = frame
	app.machine.replay_finished.connect(on_finished)
	error = app.machine.start_replay(FRAMES - 1, true)
	if not error.is_empty():
		push_error("Replay start: " + error)
		app.machine.replay_finished.disconnect(on_finished)
		return false
	var deadline := Time.get_ticks_msec() + 120000
	while finished[0] < 0 and Time.get_ticks_msec() < deadline:
		await process_frame
	app.machine.replay_finished.disconnect(on_finished)
	if finished[0] != FRAMES - 1:
		push_error("Replay did not finish within 120 seconds")
		return false
	var rows: PackedStringArray = app.machine.get_replay_hashes()
	if rows.size() != FRAMES:
		push_error("Replay did not record all 600 hashes")
		return false
	var file := FileAccess.open(output, FileAccess.WRITE)
	if file == null: return false
	for row in rows: file.store_line(row)
	file.close()
	return app.machine.flush_save()

func _run() -> void:
	var args := OS.get_cmdline_user_args()
	var rom := arg(args, "--rom")
	var save_a := arg(args, "--save-a")
	var save_b := arg(args, "--save-b")
	var language := arg(args, "--language")
	var epoch := arg(args, "--rtc-epoch")
	var timezone := arg(args, "--rtc-timezone-offset")
	var input := FileAccess.open(arg(args, "--input"), FileAccess.READ)
	if rom.is_empty() or save_a.is_empty() or save_b.is_empty() or language.is_empty() or not epoch.is_valid_int() or not timezone.is_valid_int() or input == null:
		fail("invalid arguments"); return
	var inputs := input.get_as_text().split("\n")
	input.close()
	var initial := Logic.DEFAULTS.duplicate(true)
	initial.rom_path = rom
	initial.save_path = save_a
	initial.language = language
	initial.launcher_live_background = false
	initial.renderer_gpu3d = 0
	Logic.save_settings("user://settings.cfg", initial)
	var app = load("res://scenes/main.tscn").instantiate()
	root.add_child(app)
	await process_frame
	app.attract.live_started.connect(func(): events.live += 1)
	app.attract.frame_ready.connect(func(_top, _bottom): events.frames += 1)
	app.attract.released.connect(func(): events.released += 1)
	await app._start_game()
	if app.machine == null or app.attract.active:
		fail("baseline game start"); return
	if not await replay(app, rom, save_a, language, int(epoch), int(timezone), inputs, arg(args, "--hash-a")):
		fail("baseline replay"); return
	app._release_machine(false)
	app.settings.save_path = save_b
	app.settings.launcher_live_background = true
	# Attract prefers GPU even when gameplay is configured for software.
	app.settings.renderer_gpu3d = 0
	app.settings.gpu3d_scale = 4
	app._show_launcher()
	if not app.attract.active:
		fail("attract did not start at title"); return
	if not app.attract._machine.get_gpu3d_enabled() or app.attract._gpu_scale != 4:
		fail("attract did not apply GPU renderer and scale"); return
	var disposable: String = app.attract._save_path
	if disposable.is_empty() or disposable == save_a or disposable == save_b:
		fail("attract save is not isolated"); return
	if not await wait_live(app.attract, 0):
		fail("attract did not produce live frames within 120 seconds"); return
	app.attract.set_gpu3d_settings(false, 4)
	var live_material := app.launcher._live_frame.material as ShaderMaterial
	if app.attract._machine.get_gpu3d_enabled() or bool(live_material.get_shader_parameter("use_gpu")):
		fail("attract software fallback was not selected"); return
	app.attract.set_gpu3d_settings(true, 4)
	if not app.attract._machine.get_gpu3d_enabled():
		fail("attract GPU renderer did not re-enable"); return
	var attract_machine_id: int = app.attract._machine.get_instance_id()
	await app._start_game()
	if app.machine == null or app.attract.active or events.released != 1 or is_instance_valid(instance_from_id(attract_machine_id)) or FileAccess.file_exists(disposable):
		fail("game started before attract was released and its save removed"); return
	if not await replay(app, rom, save_b, language, int(epoch), int(timezone), inputs, arg(args, "--hash-b")):
		fail("post-attract replay"); return
	app._stop_game()
	if app.machine != null or not app.attract.active:
		fail("quit to title did not restart attract"); return
	var restarted_save: String = app.attract._save_path
	var prior: int = events.live
	events.frames = 0
	if not await wait_live(app.attract, prior):
		fail("restarted attract did not produce live frames within 120 seconds"); return
	var restarted_machine_id: int = app.attract._machine.get_instance_id()
	app.notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
	if app.attract != null or app.launcher != null or is_instance_valid(instance_from_id(restarted_machine_id)) or FileAccess.file_exists(restarted_save):
		fail("window close did not release the live attract machine and launcher"); return
	print("PASS: attract live, release before gameplay, and window-close teardown; 600 frame hashes written")
