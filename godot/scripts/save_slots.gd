extends RefCounted
# Save slots are launcher metadata, not save states. Call only while the game is
# stopped. A legacy slot references its original file; it is never moved or
# deleted by this manager. All mutation methods return an empty error on success.
# create_slot/migrate_legacy/import_slot return {id, error}; get_slot returns {}
# for an unknown id. update_playtime adds elapsed seconds after a session.
# create_manager(machine) returns a Control with slot_selected(id, path) and
# restore_requested(id, backup_path) signals. The caller must only pass a machine
# loaded for the selected slot and perform the native safe restore itself.

const Logic = preload("res://scripts/host_logic.gd")
var root: String

func _init(directory := "user://save_slots") -> void:
	root = ProjectSettings.globalize_path(directory).simplify_path()

func _valid_id(id: String) -> bool:
	return not id.is_empty() and id != "." and id != ".." and not id.contains("/") and not id.contains("\\")

func get_slot(id: String) -> Dictionary:
	if not _valid_id(id): return {}
	var file := root.path_join(id).path_join("slot.json")
	if not FileAccess.file_exists(file): return {}
	var data = JSON.parse_string(FileAccess.get_file_as_string(file))
	if not data is Dictionary or str(data.get("id", "")) != id: return {}
	if str(data.get("path", "")).is_empty(): return {}
	return data

func list_slots() -> Array[Dictionary]:
	var result: Array[Dictionary] = []
	var dir := DirAccess.open(root)
	if dir == null: return result
	for id in dir.get_directories():
		var slot := get_slot(id)
		if not slot.is_empty(): result.append(slot)
	result.sort_custom(func(a: Dictionary, b: Dictionary): return str(a.get("updated", "")) > str(b.get("updated", "")) if a.get("updated", "") != b.get("updated", "") else str(a.id) > str(b.id))
	return result

func _write_slot(slot: Dictionary) -> String:
	var directory := root.path_join(str(slot.id))
	var error := DirAccess.make_dir_recursive_absolute(directory)
	if error != OK: return "Cannot create slot directory (%s)." % error
	var temp := directory.path_join("slot.json.tmp")
	var file := FileAccess.open(temp, FileAccess.WRITE)
	if file == null: return "Cannot write slot metadata."
	file.store_string(JSON.stringify(slot, "\t"))
	var write_error := file.get_error()
	file.close()
	if write_error != OK:
		DirAccess.remove_absolute(temp)
		return "Cannot write slot metadata (%s)." % write_error
	error = DirAccess.rename_absolute(temp, directory.path_join("slot.json"))
	if error != OK:
		DirAccess.remove_absolute(temp)
		return "Cannot replace slot metadata (%s)." % error
	return ""

func _new_slot(name: String, source: String, legacy: bool) -> Dictionary:
	name = name.strip_edges()
	if name.is_empty(): return {"id": "", "error": "Enter a slot name."}
	if not source.is_empty() and not FileAccess.file_exists(source): return {"id": "", "error": "Save file not found."}
	var id := "%d-%d" % [int(Time.get_unix_time_from_system()), Time.get_ticks_usec()]
	while DirAccess.dir_exists_absolute(root.path_join(id)): id += "-1"
	var directory := root.path_join(id)
	var error := DirAccess.make_dir_recursive_absolute(directory)
	if error != OK: return {"id": "", "error": "Cannot create slot directory (%s)." % error}
	var path := ProjectSettings.globalize_path(source).simplify_path() if legacy else directory.path_join("save.sav")
	if not source.is_empty() and not legacy:
		error = DirAccess.copy_absolute(ProjectSettings.globalize_path(source), path)
		if error != OK:
			DirAccess.remove_absolute(path); DirAccess.remove_absolute(directory)
			return {"id": "", "error": "Cannot import save (%s)." % error}
	var now := Time.get_datetime_string_from_system(true) + "Z"
	var slot := {"id": id, "name": name, "path": path, "legacy": legacy, "created": now, "updated": now, "playtime_seconds": 0}
	var message := _write_slot(slot)
	if not message.is_empty():
		if not legacy: DirAccess.remove_absolute(path)
		DirAccess.remove_absolute(directory)
		return {"id": "", "error": message}
	return {"id": id, "error": ""}

func create_slot(name: String) -> Dictionary:
	return _new_slot(name, "", false)

func migrate_legacy(path: String, name := "Existing save") -> Dictionary:
	if path.is_empty(): return {"id": "", "error": "Choose an existing save."}
	var absolute := ProjectSettings.globalize_path(path).simplify_path()
	for slot in list_slots():
		if ProjectSettings.globalize_path(str(slot.path)).simplify_path() == absolute:
			return {"id": str(slot.id), "error": ""}
	return _new_slot(name, path, true)

func import_slot(path: String, name: String) -> Dictionary:
	if path.is_empty(): return {"id": "", "error": "Choose a save to import."}
	return _new_slot(name, path, false)

func export_slot(id: String, destination: String) -> String:
	var slot := get_slot(id)
	if slot.is_empty(): return "Slot not found."
	if destination.is_empty(): return "Choose an export destination."
	var target := ProjectSettings.globalize_path(destination).simplify_path()
	for other in list_slots():
		if target == ProjectSettings.globalize_path(str(other.path)).simplify_path(): return "Cannot overwrite a slot's save file."
	if not FileAccess.file_exists(str(slot.path)): return "The slot has no save file yet."
	var error := DirAccess.copy_absolute(str(slot.path), target)
	return "" if error == OK else "Cannot export save (%s)." % error

func delete_slot(id: String) -> String:
	var slot := get_slot(id)
	if slot.is_empty(): return "Slot not found."
	# Only the fixed managed filename is owned; never trust an arbitrary path in metadata.
	var directory := root.path_join(id)
	var managed := directory.path_join("save.sav")
	if not bool(slot.get("legacy", false)) and FileAccess.file_exists(managed):
		var error := DirAccess.remove_absolute(managed)
		if error != OK: return "Cannot delete slot save (%s)." % error
	var error := DirAccess.remove_absolute(directory.path_join("slot.json"))
	if error != OK: return "Cannot delete slot metadata (%s)." % error
	# Native backup files, if present, are deliberately retained.
	DirAccess.remove_absolute(directory)
	return ""

func update_playtime(id: String, seconds: int) -> String:
	var slot := get_slot(id)
	if slot.is_empty(): return "Slot not found."
	if seconds < 0: return "Playtime cannot be negative."
	slot["playtime_seconds"] = maxi(0, int(slot.get("playtime_seconds", 0))) + seconds
	slot["updated"] = Time.get_datetime_string_from_system(true) + "Z"
	return _write_slot(slot)

# Machine must be loaded for this slot; native code owns backup discovery and
# restore. The helpers retain native order and enforce its capacity hints.
func backup_rows(id: String, machine: Node) -> Array[Dictionary]:
	if get_slot(id).is_empty() or machine == null or not machine.has_method("get_save_backups") or not machine.has_method("get_save_capacity"): return []
	return Logic.save_backup_rows(machine.get_save_backups(), int(machine.get_save_capacity()))

func create_manager(machine: Node = null) -> Control:
	return Manager.new(self, machine)

class Manager extends VBoxContainer:
	signal slot_selected(id: String, path: String)
	signal restore_requested(id: String, backup_path: String)
	signal closed
	var store
	var machine: Node
	var cards: VBoxContainer
	var card_buttons: Array[Button] = []
	var card_ids: Array[String] = []
	var detail: Label
	var empty_label: Label
	var new_button: Button
	var back_button: Button
	var backups: ItemList
	var message: Label
	var name_edit: LineEdit
	var selected_id := ""

	func _init(owner_store, backup_machine: Node = null) -> void:
		store = owner_store
		machine = backup_machine
		name = "SaveSlotsManager"
		add_theme_constant_override("separation", 12)
		var title := Label.new(); title.text = "Your journeys"; title.add_theme_font_size_override("font_size", 26); add_child(title)
		var subtitle := Label.new(); subtitle.text = "Choose a journey or begin a new one."; add_child(subtitle)
		var body := HBoxContainer.new(); body.size_flags_vertical = Control.SIZE_EXPAND_FILL; body.add_theme_constant_override("separation", 20); add_child(body)
		var journeys := VBoxContainer.new(); journeys.size_flags_horizontal = Control.SIZE_EXPAND_FILL; body.add_child(journeys)
		var journeys_label := Label.new(); journeys_label.text = "JOURNEYS"; journeys.add_child(journeys_label)
		var scroll := ScrollContainer.new(); scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL; scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED; scroll.follow_focus = true; journeys.add_child(scroll)
		cards = VBoxContainer.new(); cards.name = "JourneyCards"; cards.size_flags_horizontal = Control.SIZE_EXPAND_FILL; scroll.add_child(cards)
		empty_label = Label.new(); empty_label.text = "No journeys yet. Create or import a save to begin."; empty_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART; journeys.add_child(empty_label)
		var details := VBoxContainer.new(); details.size_flags_horizontal = Control.SIZE_EXPAND_FILL; details.size_flags_vertical = Control.SIZE_EXPAND_FILL; details.add_theme_constant_override("separation", 8); body.add_child(details)
		var detail_title := Label.new(); detail_title.text = "JOURNEY DETAILS"; details.add_child(detail_title)
		detail = Label.new(); detail.name = "JourneyDetail"; detail.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART; details.add_child(detail)
		name_edit = LineEdit.new(); name_edit.placeholder_text = "Name for a new journey"; details.add_child(name_edit)
		var row := HBoxContainer.new(); details.add_child(row)
		new_button = _button(row, "New journey", _create)
		_button(row, "Import…", _import_dialog)
		var actions := HBoxContainer.new(); details.add_child(actions)
		_button(actions, "Export…", _export_dialog)
		_button(actions, "Delete…", _delete_dialog)
		var label := Label.new(); label.text = "BACKUPS · SELECTED LOADED SAVE"; details.add_child(label)
		backups = ItemList.new(); backups.name = "JourneyBackups"; backups.custom_minimum_size.y = 85; backups.size_flags_vertical = Control.SIZE_EXPAND_FILL; details.add_child(backups)
		var backup_row := HBoxContainer.new(); details.add_child(backup_row)
		_button(backup_row, "Refresh backups", refresh_backups)
		_button(backup_row, "Restore…", _restore_dialog)
		back_button = _button(self, "Back", func(): closed.emit())
		back_button.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
		message = Label.new(); message.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART; add_child(message)
		refresh()

	func _ready() -> void:
		visibility_changed.connect(_focus_when_visible)
		_focus_when_visible()

	func _focus_when_visible() -> void:
		if is_visible_in_tree(): call_deferred("_focus_current_card")

	func _focus_current_card() -> void:
		if not is_inside_tree() or not is_visible_in_tree(): return
		var target: Control = new_button
		for index in card_ids.size():
			if card_ids[index] == selected_id: target = card_buttons[index]; break
		if selected_id.is_empty() and not card_buttons.is_empty(): target = card_buttons[0]
		if target.is_inside_tree(): target.grab_focus()

	func _unhandled_input(event: InputEvent) -> void:
		if is_visible_in_tree() and event.is_action_pressed("ui_cancel"):
			for child in get_children():
				if child is Window and child.visible: return
			get_viewport().set_input_as_handled()
			closed.emit()

	func _button(parent: Control, text: String, action: Callable) -> Button:
		var button := Button.new(); button.text = text; button.size_flags_horizontal = Control.SIZE_EXPAND_FILL; parent.add_child(button); button.pressed.connect(action)
		return button

	func refresh() -> void:
		for child in cards.get_children():
			cards.remove_child(child)
			child.queue_free()
		card_buttons.clear()
		card_ids.clear()
		if store.get_slot(selected_id).is_empty(): selected_id = ""
		for slot in store.list_slots():
			var seconds := int(slot.get("playtime_seconds", 0))
			var id := str(slot.id)
			var card := Button.new()
			card.name = "JourneyCard"
			card.text = "%s\n%s  ·  %dh %02dm%s" % [slot.get("name", ""), slot.get("updated", ""), seconds / 3600, (seconds / 60) % 60, "  ·  Existing" if slot.get("legacy", false) else ""]
			card.alignment = HORIZONTAL_ALIGNMENT_LEFT
			card.custom_minimum_size.y = 72
			card.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			card.toggle_mode = true
			card.button_pressed = id == selected_id
			cards.add_child(card)
			card_ids.append(id)
			card_buttons.append(card)
			card.pressed.connect(_select.bind(card_ids.size() - 1))
		for index in card_buttons.size():
			var card := card_buttons[index]
			card.focus_neighbor_top = card.get_path_to(card_buttons[maxi(0, index - 1)])
			card.focus_neighbor_bottom = card.get_path_to(card_buttons[mini(card_buttons.size() - 1, index + 1)])
			card.focus_neighbor_right = card.get_path_to(name_edit)
		if not card_buttons.is_empty():
			var selected_index := card_ids.find(selected_id)
			name_edit.focus_neighbor_left = name_edit.get_path_to(card_buttons[maxi(0, selected_index)])
		else:
			name_edit.focus_neighbor_left = NodePath()
		empty_label.visible = card_ids.is_empty()
		_update_detail()
		refresh_backups()
		if is_inside_tree(): _focus_when_visible()

	func _update_detail() -> void:
		var slot: Dictionary = store.get_slot(selected_id)
		if slot.is_empty():
			detail.text = "Select a journey to see its details and backups."
			return
		var seconds := int(slot.get("playtime_seconds", 0))
		detail.text = "%s\nLast played: %s\nPlaytime: %dh %02dm\n%s" % [slot.get("name", ""), slot.get("updated", ""), seconds / 3600, (seconds / 60) % 60, "Existing save (original retained)" if slot.get("legacy", false) else "Managed save"]

	func _select(index: int) -> void:
		selected_id = card_ids[index]
		name_edit.focus_neighbor_left = name_edit.get_path_to(card_buttons[index])
		for i in card_buttons.size(): card_buttons[i].set_pressed_no_signal(i == index)
		_update_detail()
		# A machine supplied for another slot must never be used for this one.
		machine = null
		backups.clear()
		var slot: Dictionary = store.get_slot(selected_id)
		if not slot.is_empty(): slot_selected.emit(selected_id, str(slot.path))

	# Launcher may attach a machine only after loading the selected slot.
	func set_backup_machine(value: Node) -> void:
		machine = value
		refresh_backups()

	func refresh_backups() -> void:
		backups.clear()
		for row in store.backup_rows(selected_id, machine):
			backups.add_item(str(row.label) + (" — incompatible" if not str(row.error).is_empty() else ""))
			backups.set_item_metadata(backups.item_count - 1, row)

	func _create() -> void:
		var result: Dictionary = store.create_slot(name_edit.text)
		message.text = str(result.error)
		if message.text.is_empty(): _select_new(str(result.id))

	func _select_new(id: String) -> void:
		selected_id = id
		machine = null
		refresh()
		var slot: Dictionary = store.get_slot(id)
		if not slot.is_empty(): slot_selected.emit(id, str(slot.path))

	func _confirm(text: String, action: Callable) -> void:
		var dialog := ConfirmationDialog.new(); dialog.dialog_text = text; dialog.dialog_autowrap = true; add_child(dialog)
		dialog.confirmed.connect(func(): action.call(); dialog.queue_free())
		dialog.canceled.connect(dialog.queue_free)
		dialog.popup_centered_clamped(Vector2i(600, 250), 0.9)

	func _import_dialog() -> void:
		var dialog := FileDialog.new(); dialog.access = FileDialog.ACCESS_FILESYSTEM; dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE; dialog.add_filter("*.sav", "Cartridge save"); add_child(dialog)
		dialog.file_selected.connect(func(path: String):
			dialog.queue_free()
			var slot_name := name_edit.text.strip_edges()
			if slot_name.is_empty(): slot_name = path.get_file().get_basename()
			_confirm("Import a copy of %s as %s? The original will not be changed." % [path, slot_name], func():
				var result: Dictionary = store.import_slot(path, slot_name); message.text = str(result.error)
				if message.text.is_empty(): _select_new(str(result.id))))
		dialog.canceled.connect(dialog.queue_free); dialog.popup_centered_ratio(0.8)

	func _export_dialog() -> void:
		if selected_id.is_empty(): message.text = "Select a slot first."; return
		var id := selected_id
		var dialog := FileDialog.new(); dialog.access = FileDialog.ACCESS_FILESYSTEM; dialog.file_mode = FileDialog.FILE_MODE_SAVE_FILE; dialog.add_filter("*.sav", "Cartridge save"); add_child(dialog)
		dialog.file_selected.connect(func(path: String):
			dialog.queue_free()
			_confirm("Export this save to %s? An existing file will be replaced." % path, func(): message.text = store.export_slot(id, path)))
		dialog.canceled.connect(dialog.queue_free); dialog.popup_centered_ratio(0.8)

	func _delete_dialog() -> void:
		var slot: Dictionary = store.get_slot(selected_id)
		if slot.is_empty(): message.text = "Select a slot first."; return
		var id := selected_id
		var detail := "The existing save file will not be deleted." if slot.get("legacy", false) else "The slot's save file will be deleted. Existing backups are retained."
		_confirm("Delete slot %s? %s" % [slot.name, detail], func():
			message.text = store.delete_slot(id)
			if message.text.is_empty(): selected_id = ""; machine = null; refresh())

	func _restore_dialog() -> void:
		var selection := backups.get_selected_items()
		if selection.is_empty(): message.text = "Select a backup first."; return
		var row: Dictionary = backups.get_item_metadata(selection[0])
		if not str(row.error).is_empty(): message.text = str(row.error); return
		var id := selected_id
		var path := str(row.path)
		_confirm("Request restore of %s? Current progress will be replaced. The launcher must safely close the game and perform the native restore." % row.label, func():
			if id == selected_id: restore_requested.emit(id, path))
