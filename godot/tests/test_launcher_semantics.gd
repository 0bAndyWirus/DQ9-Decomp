extends SceneTree
# Run with an isolated Godot user directory in three modes: --rom <rom>;
# --rom <rom> --save <disposable copy>; DQ9_SAVE=<copy> --rom <rom>.
# Do not pass --autostart. All runs require the native extension.
const SaveSlots = preload("res://scripts/save_slots.gd")
var ok := true

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var args := OS.get_cmdline_user_args()
	var expected := OS.get_environment("DQ9_SAVE")
	for i in args.size():
		if args[i] == "--save" and i + 1 < args.size(): expected = args[i + 1]
	var base := ProjectSettings.globalize_path("user://launcher-semantics-%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(base)
	var app = load("res://scenes/main.tscn").instantiate()
	root.add_child(app)
	await process_frame
	app.slot_store = SaveSlots.new(base.path_join("slots"))
	var first: Dictionary = app.slot_store.create_slot("First")
	var second: Dictionary = app.slot_store.create_slot("Second")
	_expect(first.error.is_empty() and second.error.is_empty(), "create existing journeys")
	app._show_launcher()
	if expected.is_empty():
		_expect(not app._explicit_save, "default launcher has no explicit save override")
		app._continue_slot()
		await process_frame
		_expect(app.machine != null and not app.active_slot.is_empty() and app.settings.save_path == app.slot_store.get_slot(app.active_slot).path, "default Continue starts an existing managed journey")
	else:
		_expect(app._explicit_save and app.settings.save_path == expected, "explicit save survives launcher with existing journeys")
		app._continue_slot()
		await process_frame
		_expect(app.machine != null and app.settings.save_path == expected and app.active_slot.is_empty(), "Continue starts the explicit save rather than choosing a journey")
	app._stop_game()
	app._select_slot(str(second.id))
	var selected: Dictionary = app.slot_store.get_slot(str(second.id))
	_expect(not app._explicit_save and app.settings.save_path == selected.path, "intentional journey selection clears explicit override")
	app._continue_slot()
	await process_frame
	_expect(app.machine != null and app.active_slot == str(second.id) and app.settings.save_path == selected.path, "Continue starts the intentionally selected journey")
	app._stop_game()
	app._set_layout(1)
	app._launcher_wizard_completed("en", 0, str(first.id), "First")
	_expect(int(app.settings.layout) == 0 and app.layout == 0, "wizard preset applies layout to the live presentation")
	_expect(app.active_slot == str(first.id), "wizard intentionally selects its journey")

	# A rejected replay must leave the launcher available; an accepted replay must
	# expose the game behind the reviewer rather than the launcher overlay.
	app._show_launcher()
	app._open_issue_reviewer()
	var missing := {"path": base.path_join("missing"), "env": {"rom_path": app.settings.rom_path}, "frame": 5, "language": "en", "comment": ""}
	await app._start_issue_replay(missing)
	_expect(app.launcher.visible and app.issue_reviewer.visible, "failed replay leaves launcher and reviewer visible")
	var issue_dir := base.path_join("issue")
	DirAccess.make_dir_recursive_absolute(issue_dir)
	var save := FileAccess.open(issue_dir.path_join("session_start.sav"), FileAccess.WRITE)
	if save == null:
		_expect(false, "create replay save")
	else:
		save.close()
	var input := FileAccess.open(issue_dir.path_join("session_input.txt"), FileAccess.WRITE)
	if input == null:
		_expect(false, "create replay input")
	else:
		input.store_line("0 0 -1 -1"); input.close()
	var issue := {"path": issue_dir, "env": {"rom_path": app.settings.rom_path, "backup_type": "eeprom64k"}, "frame": 5, "language": "en", "comment": ""}
	await app._start_issue_replay(issue)
	_expect(app.machine != null and not app.launcher.visible and app.issue_reviewer.visible, "successful replay hides launcher and retains reviewer")
	app._release_machine(false)
	app.queue_free()
	await process_frame
	_remove_tree(base)
	print("PASS: launcher semantics" if ok else "FAIL: launcher semantics")
	quit(0 if ok else 1)

func _remove_tree(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null: return
	for file in dir.get_files(): DirAccess.remove_absolute(path.path_join(file))
	for child in dir.get_directories(): _remove_tree(path.path_join(child))
	DirAccess.remove_absolute(path)

func _expect(condition: bool, description: String) -> void:
	if not condition: ok = false; push_error(description)
