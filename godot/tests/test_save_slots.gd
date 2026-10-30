extends SceneTree
const SaveSlots = preload("res://scripts/save_slots.gd")
var ok := true

class BackupMachine extends Node:
	func get_save_backups() -> Array:
		return [{"path": "/synthetic/new.bak", "size": 4}, {"path": "/synthetic/old.bak", "size": 3}]
	func get_save_capacity() -> int:
		return 4

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	# A unique synthetic root: never inspect or modify the user's real saves.
	var base := ProjectSettings.globalize_path("user://slot-test-%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(base)
	var store := SaveSlots.new(base.path_join("slots"))
	var legacy := base.path_join("original.sav")
	_write(legacy, PackedByteArray([1, 2, 3, 4]))
	var original := FileAccess.get_file_as_bytes(legacy)
	var migrated: Dictionary = store.migrate_legacy(legacy, "Original")
	_expect(migrated.error.is_empty(), "legacy migration succeeds")
	var migrated_id := str(migrated.id)
	var slot: Dictionary = store.get_slot(migrated_id)
	_expect(slot.path == legacy and slot.legacy and FileAccess.get_file_as_bytes(legacy) == original, "migration references original without modification")
	_expect(store.migrate_legacy(legacy).id == migrated_id and store.list_slots().size() == 1, "migration is idempotent")
	_expect(store.update_playtime(migrated_id, 3661).is_empty(), "record session playtime")
	var reopened := SaveSlots.new(base.path_join("slots"))
	slot = reopened.get_slot(migrated_id)
	_expect(int(slot.playtime_seconds) == 3661 and not str(slot.created).is_empty() and not str(slot.updated).is_empty(), "metadata persists across instances")
	_expect(not reopened.update_playtime(migrated_id, -1).is_empty(), "negative playtime rejected")
	var created: Dictionary = reopened.create_slot("Fresh")
	_expect(created.error.is_empty(), "create slot")
	var fresh: Dictionary = reopened.get_slot(str(created.id))
	_expect(not FileAccess.file_exists(fresh.path) and str(fresh.path).begins_with(base), "new save path is private and initially absent")
	var imported: Dictionary = reopened.import_slot(legacy, "Imported")
	_expect(imported.error.is_empty(), "import succeeds")
	var imported_slot: Dictionary = reopened.get_slot(str(imported.id))
	_expect(imported_slot.path != legacy and FileAccess.get_file_as_bytes(imported_slot.path) == original, "import creates independent copy")
	_write(imported_slot.path, PackedByteArray([5, 6, 7, 8]))
	_expect(FileAccess.get_file_as_bytes(legacy) == original, "changing imported slot leaves original untouched")
	var exported := base.path_join("export.sav")
	_expect(reopened.export_slot(str(imported.id), exported).is_empty() and FileAccess.get_file_as_bytes(exported) == PackedByteArray([5, 6, 7, 8]), "export copies selected save")
	_expect(not reopened.export_slot(str(imported.id), legacy).is_empty(), "export cannot overwrite another slot")
	_expect(not reopened.export_slot(str(created.id), exported).is_empty(), "empty new slot cannot be exported")
	_expect(not reopened.import_slot(base.path_join("missing"), "Missing").error.is_empty(), "missing import rejected")
	_expect(reopened.get_slot("../original.sav").is_empty(), "invalid id rejected")
	var machine := BackupMachine.new()
	var rows: Array[Dictionary] = reopened.backup_rows(str(imported.id), machine)
	_expect(rows.size() == 2 and rows[0].path.ends_with("new.bak") and str(rows[0].error).is_empty() and not str(rows[1].error).is_empty(), "native backup order and capacity validation")
	var manager = reopened.create_manager()
	root.add_child(manager)
	var selections: Array = []
	var restores: Array = []
	manager.slot_selected.connect(func(id, path): selections.append([id, path]))
	manager.restore_requested.connect(func(id, path): restores.append([id, path]))
	_expect(manager.card_buttons.size() == 3 and not manager.empty_label.visible, "journeys rendered as cards")
	var imported_index: int = manager.card_ids.find(str(imported.id))
	manager.card_buttons[imported_index].pressed.emit()
	_expect(selections.size() == 1 and selections[0] == [str(imported.id), str(imported_slot.path)], "card selection emits slot and path")
	_expect(manager.detail.text.contains("Imported") and manager.card_buttons[imported_index].button_pressed, "selection updates detail and card")
	manager.set_backup_machine(machine)
	_expect(manager.backups.item_count == 2, "selected journey shows native backups")
	manager.backups.select(1)
	manager._restore_dialog()
	_expect(restores.is_empty() and not manager.message.text.is_empty(), "incompatible backup cannot be restored")
	manager.backups.select(0)
	manager._restore_dialog()
	_expect(restores.is_empty(), "restore requires confirmation")
	var confirmation: ConfirmationDialog
	for child in manager.get_children():
		if child is ConfirmationDialog: confirmation = child
	_expect(confirmation != null, "restore opens confirmation")
	if confirmation != null: confirmation.confirmed.emit()
	_expect(restores.size() == 1 and restores[0] == [str(imported.id), "/synthetic/new.bak"], "confirmed restore emits selected backup")
	var fresh_index: int = manager.card_ids.find(str(created.id))
	manager.card_buttons[fresh_index].pressed.emit()
	_expect(manager.machine == null and manager.backups.item_count == 0, "changing journeys detaches previous backup machine")
	_expect(manager.card_buttons[fresh_index].focus_neighbor_right == manager.card_buttons[fresh_index].get_path_to(manager.name_edit), "card navigates to journey actions")
	manager.name_edit.text = "Another journey"
	manager._create()
	_expect(manager.card_buttons.size() == 4 and selections.size() == 3 and manager.detail.text.contains("Another journey"), "new journey is selected and shown")
	manager.free()
	machine.free()
	_expect(reopened.delete_slot(str(imported.id)).is_empty() and not FileAccess.file_exists(imported_slot.path), "delete removes owned save")
	_expect(reopened.delete_slot(migrated_id).is_empty() and FileAccess.get_file_as_bytes(legacy) == original, "delete legacy reference preserves original")
	_expect(not reopened.delete_slot(migrated_id).is_empty(), "unknown slot cannot be deleted")
	_remove_tree(base)
	print("PASS: save slot persistence and operations" if ok else "FAIL: save slots")
	quit(0 if ok else 1)

func _write(path: String, bytes: PackedByteArray) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null: ok = false; return
	file.store_buffer(bytes); file.close()

func _remove_tree(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null: return
	for file in dir.get_files(): DirAccess.remove_absolute(path.path_join(file))
	for child in dir.get_directories(): _remove_tree(path.path_join(child))
	DirAccess.remove_absolute(path)

func _expect(condition: bool, description: String) -> void:
	if not condition: ok = false; push_error(description)
