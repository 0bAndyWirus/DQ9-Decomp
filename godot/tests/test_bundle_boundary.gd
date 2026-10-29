extends SceneTree
# Run with a real window and a disposable save: -- <rom> <save-copy>

class Host extends "res://scripts/main.gd":
	var directory := "user://bundle-boundary-test"
	var captured_frame := -1
	var uploaded_frame := -1
	var capture_error := ""
	var captured_paused := false
	var fail_capture := false
	func _ready() -> void:
		settings = Logic.DEFAULTS.duplicate(true)
		_build_ui()
	func _save_bundle_at_boundary() -> void:
		captured_frame = int(machine.get_stats().frame)
		captured_paused = machine.is_paused()
		var path := directory.path_join("missing/child") if fail_capture else directory
		if fail_capture:
			# A regular file cannot be used as a directory.
			path = directory.path_join("blocked/child")
		capture_error = DebugBundle.save_note(machine, path, "boundary", captured_frame - 1,
			settings.rom_path, settings.save_path, "en")

var ok := true
func check(value: bool, message: String) -> void:
	if not value:
		push_error(message)
		ok = false

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() != 2:
		push_error("usage: test_bundle_boundary.gd -- <rom> <disposable-save>")
		quit(2)
		return
	if not ClassDB.class_exists("DqMachine"):
		GDExtensionManager.load_extension("res://dq9.gdextension")
	var app := Host.new()
	root.add_child(app)
	app.settings.rom_path = args[0]
	app.settings.save_path = args[1]
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(app.directory))
	var machine = ClassDB.instantiate("DqMachine")
	app.machine = machine
	app.add_child(machine)
	machine.set_rtc_epoch(0, 0)
	var error: String = machine.load_game(args[0], args[1], 0)
	if not error.is_empty():
		push_error(error)
		quit(2)
		return
	machine.frame_ready.connect(func(number): app.uploaded_frame = number)
	machine.start()
	await create_timer(0.1).timeout
	await app._save_bundle()
	check(app.capture_error.is_empty(), "live bundle write: " + app.capture_error)
	check(app.captured_paused and not machine.is_paused(), "live capture pauses and resumes")
	check(app.uploaded_frame == app.captured_frame, "latest completed frame uploaded before capture")
	var history := FileAccess.get_file_as_string(app.directory.path_join("session_input.txt")).strip_edges().split("\n", false)
	check(history.size() == app.captured_frame, "history ends at captured frame")
	var comment := FileAccess.get_file_as_string(app.directory.path_join("comment.txt"))
	check(comment.contains("frame: %d\n" % (app.captured_frame - 1)), "bundle frame index matches history")
	var screenshot := Image.load_from_file(app.directory.path_join("screenshot_top.png"))
	check(screenshot != null and screenshot.get_data() == machine.get_top_image().get_data(), "uploaded screenshot matches captured image")
	machine.set_paused(true)
	machine.step_once()
	await app._save_bundle()
	check(app.captured_paused and machine.is_paused(), "paused capture preserves pause")
	check(app.uploaded_frame == app.captured_frame, "queued single-step consumed at boundary")
	var blocked := FileAccess.open(app.directory.path_join("blocked"), FileAccess.WRITE)
	blocked.store_string("file")
	blocked.close()
	app.fail_capture = true
	machine.set_paused(false)
	await app._save_bundle()
	check(not app.capture_error.is_empty() and not machine.is_paused(), "failed capture restores running state")
	machine.set_paused(true)
	await app._save_bundle()
	check(not app.capture_error.is_empty() and machine.is_paused(), "failed capture restores paused state")
	app._release_machine(false)
	app.free()
	for name in DirAccess.get_files_at("user://bundle-boundary-test"):
		DirAccess.remove_absolute(ProjectSettings.globalize_path("user://bundle-boundary-test".path_join(name)))
	DirAccess.remove_absolute(ProjectSettings.globalize_path("user://bundle-boundary-test"))
	print("PASS: bundle boundary" if ok else "FAIL: bundle boundary")
	quit(0 if ok else 1)
