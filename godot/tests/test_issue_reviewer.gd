extends SceneTree
# xvfb-run -a Godot --path godot --script res://tests/test_issue_reviewer.gd -- <rom> <launch-save> <inputs> <native-dq9> <output-dir> [frames-requiring-save-write]
const Store = preload("res://scripts/issue_store.gd")
const Fixture = preload("res://tests/issue_fixture.gd")
const DebugBundle = preload("res://scripts/debug_bundle.gd")
var _ok := true

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() not in [5, 6]: push_error("Expected ROM, launch save, input script, native dq9, output directory, optional frames requiring a save write"); quit(2); return
	var frame_count := int(args[5]) if args.size() == 6 else 300
	if frame_count < 1: push_error("Frame count must be positive"); quit(2); return
	var target_frame := frame_count - 1
	var require_saved_data := args.size() == 6
	var base: String = args[4]
	DirAccess.make_dir_recursive_absolute(base)
	var source_hash := FileAccess.get_sha256(args[1])
	var roots := Fixture.make_synthetic(base)
	# An empty launch save cannot infer EEPROM from its size. The recorded setting
	# must survive capture/reload, independently of the ordinary session setting.
	var fresh_save := base.path_join("fresh-session.sav")
	var fresh_file := FileAccess.open(fresh_save, FileAccess.WRITE)
	if fresh_file == null: push_error("Cannot create empty launch save"); quit(3); return
	fresh_file.close()
	var generated := Fixture.make_real_bundle(base, args[0], fresh_save, args[2], args[3], "eeprom64k", frame_count)
	if generated.has("error"): push_error(generated.error); quit(3); return
	var originals := Fixture.snapshot_originals(roots)
	var store := Store.new(roots)
	var real_issue := store.read_issue(generated.bundle)
	_expect(real_issue.env.get("backup_type") == "eeprom64k" and FileAccess.get_file_as_bytes(real_issue.path.path_join("session_start.sav")).is_empty(), "fresh fixture records explicit EEPROM 64 KB with an empty launch save")
	var native_rows := FileAccess.get_file_as_string(generated.hashes).strip_edges().split("\n")

	# Max-speed worker test: every frame hash, including frames never presented
	# to Godot, must match the independent dq9 --input-script process.
	if not ClassDB.class_exists("DqMachine"):
		GDExtensionManager.load_extension("res://dq9.gdextension")
	var worker = ClassDB.instantiate("DqMachine")
	if worker == null: push_error("DqMachine unavailable"); quit(4); return
	root.add_child(worker); worker.set_rtc_epoch(0, 0)
	worker.set_backup_type(real_issue.env.backup_type)
	var worker_save := base.path_join("worker.sav")
	DirAccess.copy_absolute(real_issue.path.path_join("session_start.sav"), worker_save)
	var error: String = worker.load_game(args[0], worker_save, 1)
	if not error.is_empty(): push_error(error); worker.free(); quit(4); return
	_expect(not worker.set_input_script(PackedStringArray(["-1 0 -1 -1"])).is_empty(), "negative input frame rejected")
	_expect(worker.set_input_script(FileAccess.get_file_as_string(real_issue.path.path_join("session_input.txt")).split("\n")).is_empty(), "set worker input script")
	var finished_frame := [-1]
	worker.replay_finished.connect(func(frame): finished_frame[0] = frame)
	_expect(worker.start_replay(target_frame, true).is_empty(), "start Max-speed worker replay")
	var deadline := Time.get_ticks_msec() + 60000
	while finished_frame[0] < 0 and Time.get_ticks_msec() < deadline:
		worker.set_buttons(0xffff); worker.set_touch(255, 191, true) # live input cannot perturb a replay
		await process_frame
	_expect(finished_frame[0] == target_frame and int(worker.get_stats().frame) == frame_count and worker.is_paused(), "worker pauses exactly after note frame, then signals with updated images")
	var worker_rows: PackedStringArray = worker.get_replay_hashes()
	_expect(worker_rows == native_rows, "%d top/bottom hashes bit-identical to native at Max speed" % frame_count)
	var hashes := FileAccess.open(base.path_join("godot-worker.hashes"), FileAccess.WRITE)
	for row in worker_rows: hashes.store_line(row)
	hashes.close()
	var frozen_frame: int = worker.get_stats().frame
	await create_timer(0.2).timeout
	_expect(int(worker.get_stats().frame) == frozen_frame, "no overshoot while waiting for main-thread presentation")
	worker.stop()
	_expect(worker.flush_save(), "flush replay save before reload")
	_expect(worker.get_backup_type() == "eeprom64k", "real worker retains explicit EEPROM chip")
	if require_saved_data:
		_expect(FileAccess.get_file_as_bytes(worker_save).size() == 64 * 1024, "real worker persists a 64 KB EEPROM save, not the 512 KB flash fallback")
	_expect(worker.load_game(args[0], worker_save, 1).is_empty(), "reload stopped worker for sparse input semantics")
	_expect(worker.set_input_script(PackedStringArray(["2 1 -1 -1", "0 2 -1 -1", "2 8 10 20", "5 0 -1 -1"])).is_empty(), "accept sparse unsorted duplicate rows")
	finished_frame[0] = -1
	_expect(worker.start_replay(5).is_empty(), "start sparse input replay")
	deadline = Time.get_ticks_msec() + 60000
	while finished_frame[0] < 0 and Time.get_ticks_msec() < deadline: await process_frame
	_expect(worker.get_input_history() == PackedStringArray(["0 2 -1 -1", "1 0 -1 -1", "2 8 10 20", "3 0 -1 -1", "4 0 -1 -1", "5 0 -1 -1"]), "worker selects input per frame, missing rows release, last duplicate wins")
	_expect(worker.get_replay_hashes().is_empty(), "normal replay avoids hash collection")
	worker.stop()
	_expect(worker.flush_save(), "flush sparse replay save before teardown")
	worker.free()

	var app = load("res://scenes/main.tscn").instantiate()
	root.add_child(app)
	for frame in 4: await process_frame
	app.issue_reviewer.store = store
	var normal_save := base.path_join("normal.sav")
	DirAccess.copy_absolute(args[1], normal_save)
	app.settings.rom_path = args[0]; app.settings.save_path = normal_save; app.settings.language = "en"
	app.settings.backup_type = "auto"
	_key(KEY_F7)
	for frame in 3: await process_frame
	_expect(app.issue_reviewer.visible, "F7 opens reviewer from launcher")
	_key(KEY_F7)
	for frame in 3: await process_frame
	_expect(not app.issue_reviewer.visible and app.launcher.visible, "F7 closes reviewer and returns to launcher")
	var entry: Button
	app._start_game()
	for frame in 8: await process_frame
	_key(KEY_F10)
	for frame in 3: await process_frame
	_expect(app.pause_panel.visible, "F10 debug panel preserved")
	app._ui.pause_tabs.current_tab = 1
	await process_frame
	for button in app.pause_panel.find_children("*", "Button", true, false):
		if button.text == "Tickets… (F7)": entry = button
	await _click(entry)
	_expect(app.issue_reviewer.visible and app.machine.is_paused(), "F10 reviewer button routed and normal game paused")
	var list_button: Button
	for button in app.issue_reviewer.find_children("*", "Button", true, false):
		if button.get_meta("issue_path", "") == real_issue.path: list_button = button
	await _click(list_button)
	_expect(app.issue_reviewer.selected.path == real_issue.path, "list row selects detail")
	_expect(app.issue_reviewer._warning.visible and app.issue_reviewer._state.text.contains("queued_audio_ms"), "audio solo/mute warning and audio key lines displayed")
	await _click(app.issue_reviewer.controls.replay)
	var replay_copy: String = app.replay_save_path
	var normal_hash := FileAccess.get_sha256(normal_save)
	_expect(replay_copy != normal_save and replay_copy != real_issue.path.path_join("session_start.sav") and FileAccess.file_exists(replay_copy), "production replay uses a temporary launch-save copy")
	await _wait_replay(app)
	_expect(app.replay_complete and app._current_frame() == target_frame and app.machine.is_paused(), "production replay reaches target and pauses")
	_expect(app.machine.get_backup_type() == "eeprom64k" and app.settings.backup_type == "auto", "production replay applies recorded chip without changing normal settings")
	var replay_capture := base.path_join("replay-capture")
	_expect(DebugBundle.save_note(app.machine, replay_capture, "Capture during issue replay", target_frame,
		args[0], replay_copy, "en").is_empty(), "capture paused production replay")
	var captured_issue := store.read_issue(replay_capture)
	_expect(captured_issue.env.get("backup_type") == "eeprom64k", "capture uses replay chip, not normal Auto setting")
	_expect(FileAccess.get_file_as_string(replay_capture.path_join("replay.sh")).contains("--backup-type 'eeprom64k'"), "native replay invocation preserves explicit chip")
	_expect(FileAccess.get_file_as_bytes(replay_capture.path_join("session_start.sav")).is_empty(), "recapture preserves empty session-start save")
	if require_saved_data:
		_expect(FileAccess.get_file_as_bytes(replay_capture.path_join("save_at_note.sav")).size() == 64 * 1024, "production replay capture persists real 64 KB EEPROM storage")
	_expect(store.read_status(real_issue.path).status == "testing", "replay enters testing, not auto-solved")
	await _click(app.issue_reviewer.controls.compare)
	var status := store.read_status(real_issue.path)
	_expect(status.last_test.result_pixels_differing == 0 and status.last_test.frame == target_frame and status.last_test.build.begins_with("dq9_godot sha256:"), "comparison captures both screens and records informational pixel count/build")
	_expect(app.issue_reviewer._comparison.text.contains("Informational"), "diff is not a pass/fail verdict")
	var changed: Image = app.issue_reviewer._current.texture.get_image()
	changed.set_pixel(0, 0, Color(1, 0, 1))
	_expect(app.issue_reviewer.show_comparison(changed) == 1, "difference image counts RGB pixels, not components")
	await _click(app.issue_reviewer.controls.compare)
	app.issue_reviewer.controls.note.text = "Retested at frame %d; original symptom is gone." % target_frame
	await _click(app.issue_reviewer.controls.solved)
	_expect(store.read_status(real_issue.path).status == "solved", "Mark solved routed and persisted")
	await _click(app.issue_reviewer.controls.reopen)
	_expect(store.read_status(real_issue.path).status == "open", "Reopen routed and persisted")
	await _click(app.issue_reviewer.controls.wontfix)
	_expect(store.read_status(real_issue.path).status == "wontfix", "Won't fix routed and persisted")
	app.issue_reviewer.controls.note.text = "A separate retest observation."
	await _click(app.issue_reviewer.controls.retest)
	_expect(store.read_status(real_issue.path).history.back().note == "A separate retest observation.", "Add retest note routed")
	await _click(app.issue_reviewer.controls.export)
	var report_dialog: Node # locate the actual FileDialog, not a mock
	for dialog in app.issue_reviewer.find_children("*", "FileDialog", true, false): report_dialog = dialog
	_expect(report_dialog is FileDialog and report_dialog.visible, "Export report opens file picker")
	if report_dialog is FileDialog:
		report_dialog.file_selected.emit(base.path_join("issue-report.txt"))
		await process_frame
	_expect(FileAccess.get_file_as_string(base.path_join("issue-report.txt")).contains("[wontfix]"), "export route writes all statuses")
	await _capture(app, Vector2i(1280, 800), base.path_join("reviewer-1280x800.png"))
	await _capture(app, Vector2i(3072, 1920), base.path_join("reviewer-3072x1920.png"))
	root.size = Vector2i(1280, 800)
	for frame in 4: await process_frame
	await _click(app.issue_reviewer.controls.play)
	_expect(not app.issue_reviewer.visible and not app.machine.is_paused(), "Play from here hands over live input")
	Input.action_press("ds_a")
	await create_timer(0.15).timeout
	Input.action_release("ds_a")
	_expect(app._current_frame() > target_frame, "live game advances beyond replay target")
	var saw_live_input := false
	for row in app.machine.get_input_history():
		var parts: PackedStringArray = row.split(" ")
		if int(parts[0]) > target_frame and (int(parts[1]) & 1) != 0: saw_live_input = true
	_expect(saw_live_input, "live buttons replace script input after replay")
	_expect(FileAccess.get_sha256(normal_save) == normal_hash, "live retest never writes normal save")
	_key(KEY_F7)
	for frame in 4: await process_frame
	await _click(app.issue_reviewer.controls["return"])
	_expect(app.machine != null and app.replay_save_path.is_empty() and not FileAccess.file_exists(replay_copy), "return cleans replay copy and reloads normal game")
	_expect(app.settings.backup_type == "auto" and app.machine.get_backup_type() == "auto", "return reloads unchanged ordinary chip setting")
	_key(KEY_F7)
	for frame in 4: await process_frame
	# A long target lets the routed cancel button interrupt a real worker replay.
	var long_issue := real_issue.duplicate(true); long_issue.frame = frame_count + 10000
	app._start_issue_replay(long_issue)
	for frame in 3: await process_frame
	var cancelled_copy: String = app.replay_save_path
	await _click(app.issue_reviewer.controls.cancel)
	_expect(app.machine == null and not FileAccess.file_exists(cancelled_copy), "cancel stops worker and cleans temporary save")
	var offered := false
	for dialog in app.find_children("*", "ConfirmationDialog", true, false):
		if dialog.visible:
			offered = true
			await _click(dialog.get_cancel_button())
	_expect(offered and app.issue_reviewer.visible, "cancel offers return to normal game or keep reviewing")
	# Reload the actual Godot capture through the production route, not a hand-made
	# metadata echo, and compare independent native output and persisted save data.
	app._start_issue_replay(captured_issue, true)
	await _wait_replay(app)
	_expect(app.replay_complete and app.machine.get_backup_type() == "eeprom64k", "captured metadata selects EEPROM on a new replay machine")
	_expect(app.machine.get_replay_hashes() == native_rows, "recaptured fresh-save replay preserves every native output hash")
	_expect(app.machine.flush_save(), "flush recaptured replay save")
	if require_saved_data:
		_expect(FileAccess.get_file_as_bytes(app.replay_save_path).size() == 64 * 1024, "reloaded capture persists EEPROM capacity")
	_expect(FileAccess.get_file_as_bytes(app.replay_save_path) == FileAccess.get_file_as_bytes(replay_capture.path_join("save_at_note.sav")), "reloaded capture preserves saved bytes")
	app._release_machine(false)
	# Legacy and malformed metadata must use Auto, never a current explicit choice.
	app.settings.backup_type = "flash512k"
	for recorded in ["missing", "", "not-a-chip"]:
		var legacy_issue := real_issue.duplicate(true)
		legacy_issue.frame = 0
		if recorded == "missing": legacy_issue.env.erase("backup_type")
		else: legacy_issue.env.backup_type = recorded
		app._start_issue_replay(legacy_issue)
		_expect(app.machine != null and app.machine.get_backup_type() == "auto", "%s backup metadata defaults to Auto" % recorded)
		_expect(app.settings.backup_type == "flash512k", "legacy replay does not change ordinary explicit setting")
		app._release_machine(false)
	# Full replay selects the recorded guest-widescreen mode before loading.
	var full_issue := real_issue.duplicate(true)
	full_issue.env.widescreen_mode = "2"
	app._start_issue_replay(full_issue)
	_expect(app.machine != null and app._active_widescreen_mode == 2, "Full issue replay selects recorded mode before load")
	await _wait_replay(app)
	_expect(app.replay_complete and app._current_frame() == target_frame, "Full issue replay reaches recorded frame")
	app._release_machine(false)
	_expect(FileAccess.get_file_as_bytes(fresh_save).is_empty(), "empty source save remains untouched")
	_expect(originals == Fixture.snapshot_originals(roots), "bundle originals unchanged; only status.json written")
	_expect(FileAccess.get_sha256(args[1]) == source_hash, "user's source launch save unchanged")
	app._release_machine(false); app.free()
	await process_frame
	if require_saved_data and _ok: print("EEPROM persistence: capture/reload each flushed 64 KB and identical saved bytes")
	print("PASS: fresh EEPROM capture/reload metadata, reviewer routes, save-copy safety, Max-speed native parity (%d frames / %d screen hashes), informational diff, statuses, cancel/return, 1280x800 + 3072x1920 captures" % [frame_count, frame_count * 2] if _ok else "FAIL: issue reviewer")
	quit(0 if _ok else 1)

func _capture(app: Control, dimensions: Vector2i, path: String) -> void:
	root.size = dimensions
	for frame in 5: await process_frame
	await RenderingServer.frame_post_draw
	root.get_texture().get_image().save_png(path)
	var viewport_rect := Rect2(Vector2.ZERO, Vector2(dimensions))
	for key in ["replay", "compare", "solved", "reopen", "wontfix", "retest", "folder", "close"]:
		_expect(viewport_rect.encloses(app.issue_reviewer.controls[key].get_global_rect()), "reviewer %s button inside %s viewport" % [key, dimensions])

func _wait_replay(app: Control) -> void:
	var deadline := Time.get_ticks_msec() + 60000
	while not app.replay_complete and app.machine != null and Time.get_ticks_msec() < deadline: await process_frame

func _click(button: Button) -> void:
	if button == null: _expect(false, "button is present"); return
	var point := button.get_global_rect().get_center()
	for pressed in [true, false]:
		var event := InputEventMouseButton.new(); event.button_index = MOUSE_BUTTON_LEFT; event.pressed = pressed; event.position = point; event.global_position = point
		if pressed: event.button_mask = MOUSE_BUTTON_MASK_LEFT
		root.push_input(event)
		await process_frame
	for frame in 3: await process_frame

func _key(code: Key) -> void:
	for pressed in [true, false]:
		var event := InputEventKey.new(); event.keycode = code; event.pressed = pressed
		root.push_input(event)

func _expect(condition: bool, message: String) -> void:
	if not condition: _ok = false; push_error(message)
