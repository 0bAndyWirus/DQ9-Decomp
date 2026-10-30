extends SceneTree
# Real-extension integration: supply a local ROM; all save bytes/captures stay in a disposable directory.
# xvfb-run -a godot --path godot --script res://tests/test_save_restore.gd -- --rom <rom>
var scene: Control
var directory: String

func _initialize() -> void:
	call_deferred("_run")

func _bytes(value: int, count := 65536) -> PackedByteArray:
	var result := PackedByteArray(); result.resize(count); result.fill(value)
	return result

func _store(path: String, data: PackedByteArray) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert(file != null)
	file.store_buffer(data); file.close()

func _read(path: String) -> PackedByteArray:
	var file := FileAccess.open(path, FileAccess.READ)
	assert(file != null)
	return file.get_buffer(file.get_length())

func _run() -> void:
	directory = OS.get_environment("DQ9_RESTORE_PROOF_DIR")
	if directory.is_empty(): directory = OS.get_temp_dir().path_join("dq9-godot-restore-%d" % Time.get_ticks_usec())
	assert(DirAccess.make_dir_recursive_absolute(directory.path_join("backups")) == OK)
	var save := directory.path_join("restore.sav")
	var older := directory.path_join("backups/restore.sav.20260101T010101.000000001Z-000001.bak")
	var newer := directory.path_join("backups/restore.sav.20260102T010101.000000001Z-000001.bak")
	var mismatch := directory.path_join("backups/restore.sav.20260103T010101.000000001Z-000001.bak")
	_store(save, _bytes(0x11)); _store(older, _bytes(0x22)); _store(newer, _bytes(0x33)); _store(mismatch, _bytes(0x44, 512))
	scene = load("res://scenes/main.tscn").instantiate(); root.add_child(scene)
	await process_frame
	# Isolate the slots exercised by this test from the launcher's default store.
	scene.slot_store = load("res://scripts/save_slots.gd").new(directory.path_join("slots"))
	var original: Dictionary = scene.slot_store.migrate_legacy(save, "Restore test")
	assert(original.error.is_empty())
	var unrelated: Dictionary = scene.slot_store.create_slot("Unrelated")
	assert(unrelated.error.is_empty())
	var unrelated_path: String = scene.slot_store.get_slot(unrelated.id).path
	scene._select_slot(original.id)
	scene.settings.backup_type = "eeprom64k"
	assert(not scene.settings.rom_path.is_empty(), "Pass --rom <local image>")
	scene._start_game()
	assert(scene.machine != null)
	scene.machine.set_paused(true)
	var listed: Array = scene.machine.get_save_backups()
	assert(listed.size() == 3)
	assert(listed[0].path == mismatch and listed[1].path == newer and listed[2].path == older)
	assert(scene.machine.get_save_capacity() == 65536)
	assert(not scene.machine.restore_save(newer).is_empty(), "A paused started session must refuse direct restore")
	assert(_read(save) == _bytes(0x11))
	# Edited settings must not redirect a running session's restore or convert its chip.
	scene._select_slot(unrelated.id)
	scene.settings.backup_type = "flash512k"
	# Capacity refusal must leave both persisted state and the live game unchanged.
	var old_id: int = scene.machine.get_instance_id()
	scene._restore_backup(mismatch)
	assert(scene.machine != null and scene.machine.get_instance_id() == old_id)
	assert(_read(save) == _bytes(0x11))
	# Confirmation is a real modal: merely opening/cancelling must not change bytes.
	scene._confirm_restore(newer)
	await process_frame
	assert(_read(save) == _bytes(0x11))
	var confirmation: ConfirmationDialog
	for child in scene.get_children():
		if child is ConfirmationDialog and child.visible: confirmation = child
	assert(confirmation != null)
	confirmation.confirmed.emit()
	if scene.machine != null: scene.machine.set_paused(true)
	await process_frame
	assert(_read(save) == _bytes(0x33), "Restore must replace persisted bytes exactly")
	assert(_read(newer) == _bytes(0x33), "Restore source must remain unchanged")
	assert(scene.machine != null and scene.machine.get_instance_id() != old_id, "Restore must restart, never replace live cartridge bytes")
	assert(scene.machine.get_save_capacity() == 65536)
	assert(not FileAccess.file_exists(unrelated_path))
	scene.machine.set_paused(true)
	scene.pause_panel.hide()
	scene._toggle_pause()
	await process_frame
	await process_frame
	var screenshot := directory.path_join("restore-panel.png")
	assert(root.get_texture().get_image().save_png(screenshot) == OK)
	scene._stop_game()
	assert(_read(save) == _bytes(0x33), "Closing after restart must preserve restored bytes")
	scene._select_slot(original.id)
	scene.settings.backup_type = "eeprom64k"
	scene._restore_backup(older)
	assert(scene.machine == null, "A stopped restore must stay stopped")
	assert(_read(save) == _bytes(0x22))
	# Enumeration errors are not an empty successful list.
	var backups_directory := directory.path_join("backups")
	var hidden_directory := directory.path_join("backups-held")
	assert(DirAccess.rename_absolute(backups_directory, hidden_directory) == OK)
	_store(backups_directory, _bytes(0, 1))
	scene._refresh_save_backups()
	assert(not scene._backup_message.is_empty() and not scene._backup_message.begins_with("No timestamped"))
	assert(DirAccess.remove_absolute(backups_directory) == OK)
	assert(DirAccess.rename_absolute(hidden_directory, backups_directory) == OK)
	print("PASS: Godot restore: 3 ordered backups, paused refusal, capacity mismatch refusal, confirmation, exact 65536-byte replacement, active-config isolation, restart/close, stopped restore and listing-error reporting; screenshot ", screenshot)
	quit(0)
