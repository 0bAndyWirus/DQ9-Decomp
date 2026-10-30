extends PanelContainer

const Store = preload("res://scripts/issue_store.gd")
signal replay_requested(issue: Dictionary)
signal compare_requested(issue: Dictionary)
signal view_requested(issue: Dictionary)
signal play_requested
signal cancel_requested
signal return_requested
signal closed
signal preferences_changed(show_auto: bool)

var store = Store.new()
var selected: Dictionary = {}
var controls: Dictionary = {}
var _issues: Array[Dictionary] = []
var _list: VBoxContainer
var _split: HSplitContainer
var _list_scroll: ScrollContainer
var _detail: VBoxContainer
var _ui_scale := 1.0
var _header: Label
var _summary: Label
var _comment: Label
var _warning: Label
var _state: Label
var _history: Label
var _message: Label
var _search: LineEdit
var _filter: OptionButton
var _auto: CheckButton
var _note: TextEdit
var _progress: ProgressBar
var _progress_label: Label
var _original: TextureRect
var _current: TextureRect
var _difference: TextureRect
var _comparison: Label
var _busy := false
var _replay_path := ""
var _replay_complete := false

func _ready() -> void:
	name = "IssueReviewer"
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	offset_left = 24; offset_right = -24; offset_top = 24; offset_bottom = -24
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.045, 0.06, 0.085)
	style.border_color = Color(0.3, 0.65, 0.95)
	style.set_border_width_all(2); style.set_content_margin_all(16)
	style.set_corner_radius_all(8)
	add_theme_stylebox_override("panel", style)
	var column := VBoxContainer.new(); column.add_theme_constant_override("separation", 10); add_child(column)
	var heading := HBoxContainer.new(); column.add_child(heading)
	_header = _label("ISSUE REVIEWER", heading)
	_header.add_theme_font_size_override("font_size", 24); _header.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_button("refresh", "Refresh", heading, refresh)
	_button("export", "Export report…", heading, _export_report)
	_button("close", "Close (F7)", heading, func(): closed.emit())
	var filters := HBoxContainer.new(); column.add_child(filters)
	_filter = OptionButton.new()
	for text in ["All issues", "Open / testing", "Solved"]: _filter.add_item(text)
	filters.add_child(_filter); _filter.item_selected.connect(func(_index): refresh())
	_search = LineEdit.new(); _search.placeholder_text = "Search comments…"; _search.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	filters.add_child(_search); _search.text_changed.connect(func(_text): refresh())
	_auto = CheckButton.new(); _auto.text = "Show auto notes"; filters.add_child(_auto)
	_auto.toggled.connect(func(value): preferences_changed.emit(value); refresh())
	controls.search = _search; controls.filter = _filter; controls.show_auto = _auto
	_split = HSplitContainer.new(); _split.size_flags_vertical = Control.SIZE_EXPAND_FILL; column.add_child(_split)
	_list_scroll = ScrollContainer.new(); _list_scroll.size_flags_horizontal = Control.SIZE_FILL; _split.add_child(_list_scroll)
	_list = VBoxContainer.new(); _list.size_flags_horizontal = Control.SIZE_EXPAND_FILL; _list_scroll.add_child(_list)
	var detail := VBoxContainer.new(); detail.size_flags_horizontal = Control.SIZE_EXPAND_FILL; _split.add_child(detail)
	_detail = detail
	_summary = _label("Select a previous issue", detail); _summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	var replay_row := HFlowContainer.new(); detail.add_child(replay_row)
	_button("replay", "Replay to issue", replay_row, func(): replay_requested.emit(selected))
	_button("compare", "Compare with original", replay_row, func(): compare_requested.emit(selected))
	_button("view", "View with my GPU/widescreen settings", replay_row, func(): view_requested.emit(selected))
	_button("play", "Play from here", replay_row, func(): play_requested.emit())
	_button("cancel", "Cancel replay", replay_row, func(): cancel_requested.emit())
	_button("return", "Return to normal game", replay_row, func(): return_requested.emit())
	_progress = ProgressBar.new(); _progress.show_percentage = true; detail.add_child(_progress)
	_progress_label = _label("", detail); _progress_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	var status_row := HFlowContainer.new(); detail.add_child(status_row)
	_button("solved", "Mark solved", status_row, func(): _set_status("solved"))
	_button("reopen", "Reopen", status_row, func(): _set_status("open"))
	_button("wontfix", "Won't fix", status_row, func(): _set_status("wontfix"))
	_button("retest", "Add retest note", status_row, _add_retest)
	_button("folder", "Open bundle folder", status_row, func(): OS.shell_open(selected.path))
	_note = TextEdit.new(); _note.placeholder_text = "Optional resolution note / retest observations"; _note.custom_minimum_size.y = 56; detail.add_child(_note)
	controls.note = _note
	_message = _label("", detail); _message.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	var detail_scroll := ScrollContainer.new(); detail_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL; detail.add_child(detail_scroll)
	var evidence := VBoxContainer.new(); evidence.size_flags_horizontal = Control.SIZE_EXPAND_FILL; detail_scroll.add_child(evidence)
	_comment = _label("", evidence); _comment.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_warning = _label("", evidence); _warning.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART; _warning.add_theme_color_override("font_color", Color(1, 0.7, 0.25))
	_comparison = _label("Original screenshots (top / bottom)", evidence); _comparison.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	var images := HBoxContainer.new(); evidence.add_child(images)
	_original = _image_column("Original", images)
	_current = _image_column("Current", images)
	_difference = _image_column("Difference (RGB)", images)
	_state = _label("", evidence); _state.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_history = _label("", evidence); _history.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	theme = Theme.new()
	get_viewport().size_changed.connect(_resize_ui)
	_resize_ui()
	_update_buttons()

func _resize_ui() -> void:
	_ui_scale = clampf(get_viewport_rect().size.y / 800.0, 1.0, 2.0)
	theme.default_font_size = roundi(16 * _ui_scale)
	_header.add_theme_font_size_override("font_size", roundi(24 * _ui_scale))
	_list_scroll.custom_minimum_size.x = 290 * _ui_scale
	_detail.custom_minimum_size.x = 580 * _ui_scale
	# split_offset is relative to the first child's base size, not the viewport.
	_split.split_offset = roundi(40 * _ui_scale)
	_note.custom_minimum_size.y = 56 * _ui_scale
	for image in [_original, _current, _difference]: image.custom_minimum_size = Vector2(150, 256) * _ui_scale
	refresh()


func open(show_auto := false) -> void:
	_auto.set_pressed_no_signal(show_auto)
	show(); refresh()
	if not controls.view.disabled: controls.view.grab_focus()
	elif not controls.replay.disabled: controls.replay.grab_focus()
	else: controls.close.grab_focus()

func refresh() -> void:
	var all: Array[Dictionary] = store.list_issues("all", "", _auto.button_pressed)
	var open_count := 0
	var solved_count := 0
	for issue in all:
		if issue.status.status in ["open", "testing"]: open_count += 1
		if issue.status.status == "solved": solved_count += 1
	_header.text = "ISSUE REVIEWER  •  %d open  /  %d solved" % [open_count, solved_count]
	_issues = store.list_issues(["all", "open", "solved"][_filter.selected], _search.text, _auto.button_pressed)
	for child in _list.get_children(): child.free()
	for issue in _issues:
		var row := HBoxContainer.new(); _list.add_child(row)
		var thumb := TextureRect.new(); thumb.custom_minimum_size = Vector2(60, 90) * _ui_scale; thumb.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
		thumb.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED; thumb.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
		thumb.texture = _load_texture(issue.thumbnail); row.add_child(thumb)
		var button := Button.new(); button.text = "[%s]  %s\nFrame %d • %s\n%s" % [issue.status.status.to_upper(), issue.date.replace("T", " "), issue.frame, issue.language, issue.note.replace("\n", " ").left(72)]
		button.alignment = HORIZONTAL_ALIGNMENT_LEFT; button.clip_text = true; button.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
		button.size_flags_horizontal = Control.SIZE_EXPAND_FILL; button.custom_minimum_size.y = 90 * _ui_scale; button.tooltip_text = issue.note
		button.set_meta("issue_path", issue.path); button.pressed.connect(func(): select_issue(issue)); row.add_child(button)
	if _issues.is_empty(): _label("No matching issues.\nBoth SDL and Godot bundle folders are searched.", _list).autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	if not selected.is_empty():
		selected = store.read_issue(selected.path)
		_show_detail()
	elif not _issues.is_empty(): select_issue(_issues[0])

func select_issue(issue: Dictionary) -> void:
	selected = store.read_issue(issue.path)
	_note.text = ""; _message.text = ""
	_current.texture = null; _difference.texture = null
	_comparison.text = "Original screenshots (top / bottom)"
	_show_detail()

func _show_detail() -> void:
	_summary.text = "[%s]  %s  •  Frame %d  •  %s" % [selected.status.status.to_upper(), selected.date.replace("T", " "), selected.frame, selected.language]
	_comment.text = selected.note
	_warning.text = selected.audio_warning; _warning.visible = not _warning.text.is_empty()
	_original.texture = _load_texture(selected.thumbnail)
	var state_lines := PackedStringArray()
	for line in selected.state.split("\n"):
		if state_lines.size() >= 16: break
		if not line.is_empty(): state_lines.append(line)
	var audio_lines := PackedStringArray()
	for line in selected.audio.split("\n"):
		var lower: String = line.to_lower()
		if ["queue", "underrun", "drop", "clip", "frame", "rate", "rms", "peak", "driver", "device"].any(func(key): return lower.contains(key)): audio_lines.append(line)
	_state.text = "STATE SUMMARY\n" + "\n".join(state_lines) + "\n\nAUDIO STATS\n" + ("\n".join(audio_lines) if not audio_lines.is_empty() else "No audio_stats.txt in this bundle. See state summary for host audio stats.")
	var entries := PackedStringArray(["REVIEW HISTORY"])
	for entry in selected.status.history:
		if entry is Dictionary: entries.append("%s  [%s]  %s" % [entry.get("time", ""), entry.get("status", ""), entry.get("note", "")])
	if not selected.status.last_test.is_empty(): entries.append("Last test: " + JSON.stringify(selected.status.last_test))
	_history.text = "\n".join(entries)
	_update_buttons()

func set_replay_state(path: String, busy: bool, complete := false) -> void:
	_replay_path = path; _busy = busy; _replay_complete = complete
	if busy: _progress.value = 0; _progress_label.text = "Replaying deterministically at Max speed…"
	elif complete:
		_progress.value = 100
		_progress_label.text = "Paused at the issue. Compare, view with your GPU/widescreen settings, play from here, or return to the normal game. Changing to or from Full requires a fresh replay."
	else: _progress_label.text = "Replay cancelled. Return to normal game or select another issue." if not path.is_empty() else ""
	_update_buttons()

func update_progress(frame_count: int, target_frame: int, elapsed_seconds: float) -> void:
	var total := target_frame + 1
	_progress.value = 100.0 * frame_count / maxf(1.0, total)
	var rate := frame_count / maxf(0.001, elapsed_seconds)
	var eta := (total - frame_count) / maxf(0.001, rate)
	_progress_label.text = "%d / %d frames  •  %.1f frames/s  •  ETA %s" % [frame_count, total, rate, "%ds" % ceili(eta) if frame_count > 0 else "estimating…"]

func show_comparison(current: Image) -> int:
	var original := Image.load_from_file(selected.thumbnail)
	if original == null or original.is_empty() or original.get_size() != current.get_size():
		show_message("Original screenshot is missing or has different dimensions; comparison unavailable.")
		return -1
	original.convert(Image.FORMAT_RGBA8)
	var captured := current.duplicate() as Image; captured.convert(Image.FORMAT_RGBA8)
	var a := original.get_data(); var b := captured.get_data()
	var diff := PackedByteArray(); diff.resize(a.size())
	var pixels := 0
	for offset in range(0, a.size(), 4):
		var different := false
		for component in 3:
			var delta := absi(int(a[offset + component]) - int(b[offset + component]))
			diff[offset + component] = delta
			if delta != 0: different = true
		diff[offset + 3] = 255
		if different: pixels += 1
	_current.texture = ImageTexture.create_from_image(captured)
	_difference.texture = ImageTexture.create_from_image(Image.create_from_data(current.get_width(), current.get_height(), false, Image.FORMAT_RGBA8, diff))
	_comparison.text = "%d / %d pixels differ. Informational only: fixes can legitimately change the replay." % [pixels, current.get_width() * current.get_height()]
	return pixels

func show_message(message: String) -> void:
	_message.text = message

func _update_buttons() -> void:
	for key in ["replay", "solved", "reopen", "wontfix", "retest", "folder"]:
		controls[key].disabled = selected.is_empty() or _busy
	controls.compare.disabled = selected.is_empty() or _busy or not _replay_complete or selected.path != _replay_path
	controls.view.disabled = selected.is_empty() or _busy or not _replay_complete or selected.path != _replay_path
	controls.play.disabled = selected.is_empty() or _busy or not _replay_complete or selected.path != _replay_path
	controls.cancel.disabled = not _busy
	controls["return"].disabled = _replay_path.is_empty()
	controls.close.disabled = _busy

func _set_status(status: String) -> void:
	var error: String = store.set_status(selected.path, status, _note.text)
	if error.is_empty(): _note.text = ""; refresh()
	show_message(error if not error.is_empty() else "Status saved: " + status)

func _add_retest() -> void:
	var error: String = store.add_retest_note(selected.path, _note.text)
	if error.is_empty(): _note.text = ""; refresh()
	show_message(error if not error.is_empty() else "Retest note saved.")

func _export_report() -> void:
	var dialog := FileDialog.new(); dialog.access = FileDialog.ACCESS_FILESYSTEM; dialog.file_mode = FileDialog.FILE_MODE_SAVE_FILE
	dialog.add_filter("*.txt", "Issue report"); dialog.current_dir = ProjectSettings.globalize_path("user://"); dialog.current_file = "issue-report.txt"
	add_child(dialog)
	dialog.file_selected.connect(func(path):
		var error: String = store.export_report(path)
		show_message(error if not error.is_empty() else "Exported: " + path)
		dialog.queue_free())
	dialog.canceled.connect(dialog.queue_free); dialog.popup_centered_ratio(0.7)

func _button(key: String, text: String, parent: Node, callback: Callable) -> Button:
	var button := Button.new(); button.text = text; button.name = key.capitalize(); button.pressed.connect(callback); parent.add_child(button)
	controls[key] = button
	return button

static func _label(text: String, parent: Node) -> Label:
	var label := Label.new(); label.text = text; parent.add_child(label); return label

static func _image_column(title: String, parent: Node) -> TextureRect:
	var column := VBoxContainer.new(); column.size_flags_horizontal = Control.SIZE_EXPAND_FILL; parent.add_child(column)
	_label(title, column)
	var texture := TextureRect.new(); texture.expand_mode = TextureRect.EXPAND_IGNORE_SIZE; texture.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	texture.custom_minimum_size = Vector2(150, 256); texture.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	texture.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST; column.add_child(texture)
	return texture

static func _load_texture(path: String) -> Texture2D:
	if not FileAccess.file_exists(path): return null
	var image := Image.load_from_file(path)
	return ImageTexture.create_from_image(image) if image != null and not image.is_empty() else null
