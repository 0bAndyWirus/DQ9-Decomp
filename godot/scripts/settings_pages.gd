extends VBoxContainer

# Page content must not impose its full height on the modal; it scrolls instead.
class SettingsScroll extends ScrollContainer:
	func _get_minimum_size() -> Vector2:
		return Vector2.ZERO

# The owner applies and persists changes; this control never reads or writes files.
signal setting_changed(key: String, value: Variant)
signal bindings_changed
signal closed

const Logic = preload("res://scripts/host_logic.gd")
const LANGUAGES := ["en", "fr", "de", "es", "it"]
const CLASSIC := {"layout": 0, "nearest": true, "integer_scale": false, "renderer_gpu3d": 0, "gpu3d_scale": 3, "widescreen_mode": 0, "hd_textures": false}
const ENHANCED := {"layout": 0, "nearest": true, "integer_scale": false, "renderer_gpu3d": 1, "gpu3d_scale": 3, "widescreen_mode": 1, "hd_textures": false}

var _settings: Dictionary
var _running := false
var _active_widescreen := 0
var _controls: Dictionary = {}
var _binding_buttons: Dictionary = {}
var _binding_action := ""
var _binding_note: Label
var _wide_note: Label
var _preset: OptionButton
var _description: Label
var _wide_badges: Label
var _back: Button

const DESCRIPTIONS := {
	"Preset": "Choose Classic, Enhanced, or adjust individual settings for a Custom configuration.",
	"3D renderer": "Choose the original software renderer or experimental GPU hi-res rendering.",
	"GPU scale": "Set the internal resolution multiplier for GPU hi-res rendering.",
	"HD texture replacements": "Use available high-definition texture replacements.",
	"Widescreen": "Picture widens presentation only. Full changes the guest camera and requires a restart.",
	"Screen layout": "Choose how the two game screens are arranged.",
	"Nearest filter": "Keep pixels crisp when scaling the game screens.",
	"Integer scaling": "Scale screens by whole-number multiples when possible.",
	"Window scale": "Set the initial window size.",
	"Live launcher background": "Show the live game background in the launcher.",
	"Reduced menu motion": "Reduce motion and animation in the launcher menus.",
	"Volume": "Adjust game audio volume.",
	"Menu music": "Adjust launcher menu music volume.",
	"UI sound effects": "Adjust menu interaction sound effects volume.",
	"Language": "Choose the game language for the next launch.",
	"Backup memory": "Choose cartridge backup memory for the next launch; Auto uses the build setting or save size.",
	"Reset bindings": "Restore the default keyboard and controller bindings.",
}
const CATEGORY_DESCRIPTIONS := [
	"Configure rendering, widescreen, and graphics presets.",
	"Configure screen layout and presentation.",
	"Adjust game and menu audio levels.",
	"Choose keyboard and controller bindings.",
	"Choose language and cartridge backup memory.",
]


# settings is the host's live dictionary, not a copy.
func configure(settings: Dictionary, running := false, active_widescreen := 0) -> void:
	_settings = settings
	if _controls.is_empty():
		_build_pages()
	refresh(running, active_widescreen)



static func apply_preset(settings: Dictionary, index: int) -> Dictionary:
	var changed := {}
	if index != 0 and index != 1: return changed
	var preset: Dictionary = CLASSIC if index == 0 else ENHANCED
	for key in preset:
		if settings.get(key) != preset[key]:
			settings[key] = preset[key]
			changed[key] = preset[key]
	return changed
# Call after outside changes and whenever the page is reopened.
func refresh(running := false, active_widescreen := 0) -> void:
	_running = running
	_active_widescreen = active_widescreen
	_binding_action = ""
	if _controls.is_empty(): return
	for key in ["layout", "renderer_gpu3d", "widescreen_mode"]:
		(_controls[key] as OptionButton).select(int(_settings.get(key, Logic.DEFAULTS[key])))
	(_controls.gpu3d_scale as OptionButton).select(clampi(int(_settings.get("gpu3d_scale", 3)) - 2, 0, 4))
	(_controls.window_scale as OptionButton).select(clampi(int(_settings.get("window_scale", 1)) - 1, 0, 2))
	(_controls.language as OptionButton).select(maxi(0, LANGUAGES.find(_settings.get("language", "en"))))
	(_controls.backup_type as OptionButton).select(maxi(0, Logic.BACKUP_TYPES.find(_settings.get("backup_type", "auto"))))
	for key in ["nearest", "integer_scale", "hd_textures"]:
		(_controls[key] as CheckButton).set_pressed_no_signal(bool(_settings.get(key, Logic.DEFAULTS[key])))
	(_controls.launcher_live_background as CheckButton).set_pressed_no_signal(bool(_settings.get("launcher_live_background", true)))
	(_controls.menu_reduced_motion as CheckButton).set_pressed_no_signal(bool(_settings.get("menu_reduced_motion", false)))
	for key in ["volume", "menu_music_volume", "ui_sfx_volume"]:
		var fallback := 0.8 if key == "volume" else 0.7
		(_controls[key] as HSlider).set_value_no_signal(float(_settings.get(key, fallback)))
	_update_details()
	_refresh_bindings()
	_focus_tabs()


func _focus_tabs() -> void:
	if not is_inside_tree() or not is_visible_in_tree(): return
	var tabs := _controls.get("tabs") as TabContainer
	if tabs == null: return
	if not tabs.is_inside_tree():
		call_deferred("_focus_tabs")
		return
	var focused := get_viewport().gui_get_focus_owner()
	if focused == null or not is_ancestor_of(focused):
		tabs.grab_focus()


func _build_pages() -> void:
	var tabs := TabContainer.new()
	tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(tabs)
	_controls.tabs = tabs
	tabs.focus_mode = Control.FOCUS_ALL
	visibility_changed.connect(_focus_tabs)
	tabs.tab_changed.connect(_category_changed)
	tabs.focus_entered.connect(func(): _category_changed(tabs.current_tab))
	var graphics := _page(tabs, "Graphics")
	_preset = _option(graphics, "Preset", ["Classic", "Enhanced", "Custom"])
	_preset.item_selected.connect(_select_preset)
	var renderer := _option(graphics, "3D renderer", ["Software", "GPU hi-res (experimental)"])
	_controls.renderer_gpu3d = renderer
	renderer.item_selected.connect(func(i): _change("renderer_gpu3d", i))
	var scale := _option(graphics, "GPU scale", ["2x", "3x", "4x", "5x", "6x"])
	_controls.gpu3d_scale = scale
	scale.item_selected.connect(func(i): _change("gpu3d_scale", i + 2))
	_toggle(graphics, "HD texture replacements", "hd_textures")
	var widescreen := _option(graphics, "Widescreen", ["Off", "Picture", "Full"])
	_controls.widescreen_mode = widescreen
	widescreen.tooltip_text = "Picture widens presentation only. Full is experimental, changes the guest camera and is selected at game launch."
	widescreen.item_selected.connect(func(i): _change("widescreen_mode", i))
	_wide_note = Label.new()
	_wide_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	graphics.add_child(_wide_note)
	_wide_badges = Label.new()
	_wide_badges.text = "EXPERIMENTAL  ·  RESTART REQUIRED"
	graphics.add_child(_wide_badges)

	var display := _page(tabs, "Display")
	var layout := _option(display, "Screen layout", Logic.LAYOUTS)
	_controls.layout = layout
	layout.item_selected.connect(func(i): _change("layout", i))
	_toggle(display, "Nearest filter", "nearest")
	_toggle(display, "Integer scaling", "integer_scale")
	var window := _option(display, "Window scale", ["1x", "2x", "3x"])
	_controls.window_scale = window
	window.item_selected.connect(func(i): _change("window_scale", i + 1))
	_toggle(display, "Live launcher background", "launcher_live_background")
	_toggle(display, "Reduced menu motion", "menu_reduced_motion")

	var audio := _page(tabs, "Audio")
	_slider(audio, "Volume", "volume")
	_slider(audio, "Menu music", "menu_music_volume")
	_slider(audio, "UI sound effects", "ui_sfx_volume")


	var input := _page(tabs, "Controls")
	_binding_note = Label.new()
	_binding_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	input.add_child(_binding_note)
	var grid := GridContainer.new()
	grid.columns = 2
	grid.add_theme_constant_override("h_separation", 8)
	grid.add_theme_constant_override("v_separation", 8)
	input.add_child(grid)
	for action in Logic.BUTTONS:
		var button := Button.new()
		button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		button.pressed.connect(func(): _begin_binding(action))
		grid.add_child(button)
		_binding_buttons[action] = button
		_describe(button, "Select to change the binding for %s." % action.trim_prefix("ds_").capitalize())
	var reset := Button.new()
	reset.text = "Reset bindings"
	_describe(reset, DESCRIPTIONS["Reset bindings"])
	reset.pressed.connect(func():
		_binding_action = ""
		Logic.reset_bindings()
		_refresh_bindings()
		bindings_changed.emit())
	input.add_child(reset)
	var game := _page(tabs, "Language")
	var language := _option(game, "Language", ["English", "Français", "Deutsch", "Español", "Italiano"])
	_controls.language = language
	language.item_selected.connect(func(i): _change("language", LANGUAGES[i]))
	var backup := _option(game, "Backup memory", Logic.BACKUP_LABELS)
	backup.tooltip_text = "Auto uses the build setting or save size. Cartridge settings apply when starting the game."
	_controls.backup_type = backup
	backup.item_selected.connect(func(i): _change("backup_type", Logic.BACKUP_TYPES[i]))
	var game_note := Label.new()
	game_note.text = "Language and backup memory apply when the game starts."
	game_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	game.add_child(game_note)

	_description = Label.new()
	_description.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_description.text = CATEGORY_DESCRIPTIONS[0]
	add_child(_description)
	var back := Button.new()
	back.text = "Back"
	back.pressed.connect(func(): closed.emit())
	add_child(back)
	_back = back
	_update_focus_navigation()


func _input(event: InputEvent) -> void:
	if not is_visible_in_tree() or not _binding_action.is_empty(): return
	var direction := 0
	if event is InputEventJoypadButton and event.pressed:
		if event.button_index == JOY_BUTTON_LEFT_SHOULDER: direction = -1
		elif event.button_index == JOY_BUTTON_RIGHT_SHOULDER: direction = 1
	elif event is InputEventKey and event.pressed and not event.echo:
		var focus := get_viewport().gui_get_focus_owner()
		if not focus is LineEdit and not focus is TextEdit:
			if event.keycode == KEY_Q or event.keycode == KEY_PAGEUP: direction = -1
			elif event.keycode == KEY_E or event.keycode == KEY_PAGEDOWN: direction = 1
			elif event.keycode == KEY_TAB and event.ctrl_pressed: direction = -1 if event.shift_pressed else 1
	if direction != 0:
		var tabs := _controls.get("tabs") as TabContainer
		if tabs == null: return
		tabs.current_tab = posmod(tabs.current_tab + direction, tabs.get_tab_count())
		tabs.grab_focus()
		get_viewport().set_input_as_handled()


func _category_changed(index: int) -> void:
	if _description != null and index >= 0 and index < CATEGORY_DESCRIPTIONS.size():
		_description.text = CATEGORY_DESCRIPTIONS[index]
	_update_focus_navigation()


func _describe(control: Control, description: String) -> void:
	control.tooltip_text = description
	control.focus_entered.connect(func(): _description.text = description)


func _collect_focusable(node: Node, controls: Array[Control]) -> void:
	for child in node.get_children():
		if child is BaseButton:
			if not child.disabled: controls.append(child)
		elif child is HSlider:
			controls.append(child)
		else:
			_collect_focusable(child, controls)


func _update_focus_navigation() -> void:
	if _back == null: return
	var tabs := _controls.tabs as TabContainer
	var controls: Array[Control] = []
	_collect_focusable(tabs.get_tab_control(tabs.current_tab), controls)
	var first: Control = controls[0] if not controls.is_empty() else _back
	var last: Control = controls[-1] if not controls.is_empty() else tabs
	tabs.focus_neighbor_top = tabs.get_path_to(_back)
	tabs.focus_neighbor_bottom = tabs.get_path_to(first)
	tabs.focus_next = tabs.get_path_to(first)
	tabs.focus_previous = tabs.get_path_to(_back)
	_back.focus_neighbor_top = _back.get_path_to(last)
	_back.focus_neighbor_bottom = _back.get_path_to(tabs)
	_back.focus_next = _back.get_path_to(tabs)
	_back.focus_previous = _back.get_path_to(last)
	for i in controls.size():
		var control := controls[i]
		var previous: Control = tabs if i == 0 else controls[i - 1]
		var next: Control = _back if i + 1 == controls.size() else controls[i + 1]
		control.focus_neighbor_top = control.get_path_to(previous)
		control.focus_neighbor_bottom = control.get_path_to(next)
		control.focus_previous = control.get_path_to(previous)
		control.focus_next = control.get_path_to(next)
	# Bindings occupy a two-column grid; keep directional movement in the same column.
	for i in controls.size():
		var control := controls[i]
		if not control.get_parent() is GridContainer: continue
		var grid := control.get_parent() as GridContainer
		var index := control.get_index()
		if index >= grid.columns:
			control.focus_neighbor_top = control.get_path_to(grid.get_child(index - grid.columns))
		else:
			control.focus_neighbor_top = control.get_path_to(tabs)
		if index + grid.columns < grid.get_child_count():
			control.focus_neighbor_bottom = control.get_path_to(grid.get_child(index + grid.columns))
		else:
			control.focus_neighbor_bottom = control.get_path_to(controls[-1])
		# Do not let automatic spatial navigation escape the grid at its edges.
		control.focus_neighbor_left = control.get_path_to(control)
		control.focus_neighbor_right = control.get_path_to(control)
		if index % grid.columns > 0:
			control.focus_neighbor_left = control.get_path_to(grid.get_child(index - 1))
		if index % grid.columns + 1 < grid.columns and index + 1 < grid.get_child_count():
			control.focus_neighbor_right = control.get_path_to(grid.get_child(index + 1))


func _page(tabs: TabContainer, title: String) -> VBoxContainer:
	# A plain Control does not propagate its children's minimum sizes to tabs.
	var viewport := Control.new()
	viewport.name = title
	viewport.clip_contents = true
	tabs.add_child(viewport)
	var scroll := SettingsScroll.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO
	scroll.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO
	scroll.follow_focus = true
	viewport.add_child(scroll)
	scroll.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	var column := VBoxContainer.new()
	column.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	column.add_theme_constant_override("separation", 10)
	scroll.add_child(column)
	return column


func _row(parent: Control, title: String) -> HBoxContainer:
	var row := HBoxContainer.new()
	parent.add_child(row)
	var label := Label.new()
	label.text = title
	label.custom_minimum_size.x = 155
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(label)
	return row


func _option(parent: Control, title: String, entries: Array) -> OptionButton:
	var row := _row(parent, title)
	var option := OptionButton.new()
	# Keep the value column fixed even when individual option labels differ.
	option.custom_minimum_size.x = 290
	option.fit_to_longest_item = false
	for entry in entries: option.add_item(str(entry))
	_describe(option, DESCRIPTIONS[title])
	row.add_child(option)
	return option


func _toggle(parent: Control, title: String, key: String) -> void:
	var toggle := CheckButton.new()
	toggle.text = title
	parent.add_child(toggle)
	_controls[key] = toggle
	_describe(toggle, DESCRIPTIONS[title])
	toggle.toggled.connect(func(value): _change(key, value))


func _slider(parent: Control, title: String, key: String) -> void:
	var row := _row(parent, title)
	var slider := HSlider.new()
	slider.min_value = 0.0; slider.max_value = 1.0; slider.step = 0.01
	slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(slider)
	_controls[key] = slider
	var label := Label.new()
	label.custom_minimum_size.x = 48
	row.add_child(label)
	_controls[key + "_label"] = label
	_describe(slider, DESCRIPTIONS[title])
	slider.value_changed.connect(func(value): _change(key, value))


func _change(key: String, value: Variant) -> void:
	if _settings.get(key) == value: return
	_settings[key] = value
	_update_details()
	setting_changed.emit(key, value)


func _select_preset(index: int) -> void:
	if index != 0 and index != 1:
		_update_details()
		return
	var preset: Dictionary = CLASSIC if index == 0 else ENHANCED
	for key in preset:
		_change(key, preset[key])
	# The host may update active widescreen state in its signal handler.
	refresh(_running, _active_widescreen)


func _matches_preset(preset: Dictionary) -> bool:
	for key in preset:
		if _settings.get(key, Logic.DEFAULTS[key]) != preset[key]: return false
	return true


func _update_details() -> void:
	_preset.select(0 if _matches_preset(CLASSIC) else (1 if _matches_preset(ENHANCED) else 2))
	(_controls.gpu3d_scale as OptionButton).disabled = int(_settings.get("renderer_gpu3d", 0)) == 0
	_update_focus_navigation()
	for key in ["volume", "menu_music_volume", "ui_sfx_volume"]:
		var fallback := 0.8 if key == "volume" else 0.7
		(_controls[key + "_label"] as Label).text = "%d%%" % roundi(float(_settings.get(key, fallback)) * 100.0)
	var mode := int(_settings.get("widescreen_mode", 0))
	if _running and ((_active_widescreen == 2) != (mode == 2)):
		_wide_note.text = "Full is experimental and changes the guest camera. Restart the game to apply the Full mode change."
	elif mode != 0 and int(_settings.get("renderer_gpu3d", 0)) == 0:
		_wide_note.text = "Full is experimental and changes the guest camera. Widescreen requires GPU hi-res; software fallback remains 4:3."
	else:
		_wide_note.text = "Full is experimental and changes the guest camera. Changing to or from Full requires a restart."


func _begin_binding(action: String) -> void:
	_binding_action = action
	_binding_note.text = "Press a key or gamepad control for %s; Escape cancels." % action.trim_prefix("ds_").capitalize()


# Call before host hotkeys/game input. Returns true for every event while capturing.
func handle_rebind(event: InputEvent) -> bool:
	if _binding_action.is_empty() or not is_visible_in_tree(): return false
	if event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_ESCAPE:
		_binding_action = ""
		_refresh_bindings()
		return true
	var accepted: bool = (event is InputEventKey and event.pressed and not event.echo) or (event is InputEventJoypadButton and event.pressed) or (event is InputEventJoypadMotion and absf(event.axis_value) >= 0.7)
	if accepted:
		# Replace only the chosen device type, preserving keyboard or controller access.
		for old in InputMap.action_get_events(_binding_action):
			if (event is InputEventKey and old is InputEventKey) or (not event is InputEventKey and (old is InputEventJoypadButton or old is InputEventJoypadMotion)):
				InputMap.action_erase_event(_binding_action, old)
		InputMap.action_add_event(_binding_action, event)
		_binding_action = ""
		_refresh_bindings()
		bindings_changed.emit()
	return true


func _refresh_bindings() -> void:
	_binding_note.text = "Select a button, then press a key or gamepad control to rebind it."
	for action in _binding_buttons:
		var parts: Array[String] = []
		for event in InputMap.action_get_events(action):
			if event is InputEventKey:
				parts.append(event.as_text_physical_keycode() if event.physical_keycode != 0 else event.as_text_keycode())
			elif event is InputEventJoypadButton:
				parts.append("Pad %d" % event.button_index)
			elif event is InputEventJoypadMotion:
				parts.append("Axis %d%s" % [event.axis, "-" if event.axis_value < 0 else "+"])
		(_binding_buttons[action] as Button).text = "%s  [%s]" % [action.trim_prefix("ds_").capitalize(), " · ".join(parts) if not parts.is_empty() else "unbound"]
