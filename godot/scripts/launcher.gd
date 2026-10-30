extends Control

# The parent owns ROM validation, persistence, slot operations and game startup.
signal rom_selected(path: String)
signal wizard_completed(language: String, preset: int, slot_id: String, slot_name: String)
signal continue_requested
signal new_game_requested
signal settings_requested
signal slots_requested
signal quit_requested
signal back_requested

const LANGUAGES := ["en", "fr", "de", "es", "it"]

var _wizard: Control
var _title_screen: Control
var _art: TextureRect
var _game_title: Label
var _rom_label: Label
var _status: Label
var _language: OptionButton
var _preset: OptionButton
var _slot_name: LineEdit
var _continue: Button
var _finish: Button
var _browse: Button
var _dialog: FileDialog
var _rom_path := ""
var _continue_enabled := false
var _live_frame: TextureRect
var _live_tween: Tween
var _music_tween: Tween
var _music: AudioStreamPlayer
var _sfx: AudioStreamPlayer
var _music_volume := 0.7
var _sfx_volume := 0.7
var _live_music := false
var _prompts: Label
var _showing_live := false
var _reduced_motion := false
var _celestial: ShaderMaterial
var _entry_tween: Tween
var _last_title_focus: Button
var _prompt_joypad := -1
const Observatory = preload("res://scripts/ui_theme.gd")

func _exit_tree() -> void:
	for player in [_music, _sfx]:
		if is_instance_valid(player):
			player.stop()
			player.stream = null

func _setup_audio() -> void:
	for bus in ["UI", "Music"]:
		if AudioServer.get_bus_index(bus) < 0:
			AudioServer.add_bus()
			AudioServer.set_bus_name(AudioServer.bus_count - 1, bus)
	_music = AudioStreamPlayer.new()
	_music.bus = "Music"
	var track := load("res://assets/audio/menu_music.ogg") as AudioStreamOggVorbis
	if track:
		track.loop = true
		_music.stream = track
	add_child(_music)
	_sfx = AudioStreamPlayer.new()
	_sfx.bus = "UI"
	add_child(_sfx)
	_music.volume_linear = _music_volume
	if _music.stream and AudioServer.get_driver_name() != "Dummy": _music.play()
	_register_menu_sounds(get_tree().root)
	get_tree().node_added.connect(_register_menu_sounds)

func _is_other_menu(node: Node) -> bool:
	var host := get_parent()
	if host == null or not host.has_method("_show_settings"): return false
	for name in ["settings_panel", "pause_panel", "slot_manager"]:
		var panel = host.get(name)
		if is_instance_valid(panel) and panel.visible and (panel == node or panel.is_ancestor_of(node)):
			return true
	return false

func _register_menu_sounds(node: Node) -> void:
	if node is Control and (node is BaseButton or node is Range):
		var focus_action := _other_focus.bind(node)
		if not node.focus_entered.is_connected(focus_action): node.focus_entered.connect(focus_action)
		if node is BaseButton:
			var confirm_action := _other_confirm.bind(node)
			if not node.pressed.is_connected(confirm_action): node.pressed.connect(confirm_action)
	for child in node.get_children(): _register_menu_sounds(child)

func _other_focus(node: Node) -> void:
	if _is_other_menu(node): _sound("cursor")

func _other_confirm(node: Node) -> void:
	if _is_other_menu(node):
		var text: String = (node as BaseButton).text.to_lower()
		_sound("cancel" if text == "back" or text == "cancel" else "confirm")

func _sound(name: String) -> void:
	# A dummy driver cannot output sound; avoid creating needless playback objects.
	if not is_instance_valid(_sfx) or AudioServer.get_driver_name() == "Dummy": return
	_sfx.stream = load("res://assets/audio/%s.wav" % name)
	_sfx.volume_linear = _sfx_volume
	if _sfx.stream: _sfx.play()

func set_menu_music_volume(value: float) -> void:
	_music_volume = clampf(value, 0.0, 1.0)
	if is_instance_valid(_music) and not _live_music and visible:
		_music.volume_linear = _music_volume

func set_ui_sfx_volume(value: float) -> void:
	_sfx_volume = clampf(value, 0.0, 1.0)

func set_reduced_motion(value: bool) -> void:
	_reduced_motion = value
	if _celestial: _celestial.set_shader_parameter("reduced_motion", value)

func set_music_mode(live: bool) -> void:
	_live_music = live
	_update_music()

func _update_music() -> void:
	if not is_instance_valid(_music): return
	if _music_tween: _music_tween.kill()
	var muted := _live_music or not visible
	if not muted and not _music.playing and _music.stream and AudioServer.get_driver_name() != "Dummy": _music.play()
	_music_tween = create_tween()
	_music_tween.tween_property(_music, "volume_linear", 0.0 if muted else _music_volume, 0.4)
	if muted: _music_tween.tween_callback(_music.stop)

func set_live_frame(top: Texture2D, _bottom: Texture2D) -> void:
	if not is_instance_valid(_live_frame): return
	_live_frame.texture = top
	if not _showing_live:
		_showing_live = true
		if _live_tween: _live_tween.kill()
		_live_tween = create_tween()
		_live_tween.tween_property(_live_frame, "modulate:a", 1.0, 0.4)

func set_live_gpu_frame(texture: Texture2D, mask: Texture2D) -> void:
	if not is_instance_valid(_live_frame): return
	var material := _live_frame.material as ShaderMaterial
	material.set_shader_parameter("use_gpu", texture != null and mask != null)
	material.set_shader_parameter("gpu_texture", texture)
	material.set_shader_parameter("gpu_mask", mask)

func clear_live_frame() -> void:
	if not is_instance_valid(_live_frame): return
	set_live_gpu_frame(null, null)
	_showing_live = false
	if _live_tween: _live_tween.kill()
	_live_tween = create_tween()
	_live_tween.tween_property(_live_frame, "modulate:a", 0.0, 0.4)


func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	theme = Observatory.make()
	_setup_audio()
	Input.joy_connection_changed.connect(_joy_connection_changed)
	var pads := Input.get_connected_joypads()
	if not pads.is_empty(): _prompt_joypad = pads[0]
	visibility_changed.connect(_update_music)
	var background := ColorRect.new()
	background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	background.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_celestial = ShaderMaterial.new()
	_celestial.shader = load("res://shaders/ui_celestial.gdshader")
	_celestial.set_shader_parameter("reduced_motion", _reduced_motion)
	background.material = _celestial
	add_child(background)
	_live_frame = TextureRect.new()
	_live_frame.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_live_frame.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_live_frame.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_COVERED
	_live_frame.texture_filter = CanvasItem.TEXTURE_FILTER_LINEAR
	_live_frame.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_live_frame.modulate.a = 0.0
	var live_material := ShaderMaterial.new()
	live_material.shader = load("res://shaders/ui_live.gdshader")
	_live_frame.material = live_material
	add_child(_live_frame)

	var margin := MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	margin.add_theme_constant_override("margin_left", 70)
	margin.add_theme_constant_override("margin_right", 70)
	margin.add_theme_constant_override("margin_top", 70)
	margin.add_theme_constant_override("margin_bottom", 55)
	add_child(margin)
	var root := HBoxContainer.new()
	margin.add_child(root)
	var content := VBoxContainer.new()
	content.custom_minimum_size.x = 430
	content.add_theme_constant_override("separation", 12)
	root.add_child(content)
	var brand := _label("DQIX REMASTERED", content, 42)
	brand.add_theme_color_override("font_color", Observatory.IVORY)
	brand.add_theme_font_override("font", load("res://assets/fonts/Cinzel.ttf"))
	var subtitle := _label("A  C E L E S T I A L  J O U R N E Y", content, 13)
	subtitle.add_theme_color_override("font_color", Observatory.GOLD)
	var badge := HBoxContainer.new()
	badge.add_theme_constant_override("separation", 10)
	content.add_child(badge)
	_game_title = _label("Dragon Quest IX", badge, 14)
	_game_title.add_theme_color_override("font_color", Color(Observatory.IVORY, 0.65))
	_art = TextureRect.new()
	_art.custom_minimum_size = Vector2(32, 32)
	_art.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_art.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	_art.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	badge.add_child(_art)
	_wizard = VBoxContainer.new()
	_wizard.add_theme_constant_override("separation", 12)
	content.add_child(_wizard)
	_label("Welcome", _wizard, 32)
	_label("Set up your game to get started.", _wizard, 18)
	_label("Game ROM", _wizard, 18)
	var rom_row := HBoxContainer.new()
	rom_row.add_theme_constant_override("separation", 12)
	_wizard.add_child(rom_row)
	_rom_label = _label("No ROM selected", rom_row, 16)
	_rom_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_rom_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	_browse = _button("Browse…", rom_row, _pick_rom)
	_label("Language", _wizard, 18)
	_language = OptionButton.new()
	for text in ["English", "Français", "Deutsch", "Español", "Italiano"]:
		_language.add_item(text)
	_wizard.add_child(_language)
	_label("Graphics preset", _wizard, 18)
	_preset = OptionButton.new()
	for text in ["Classic", "Enhanced"]:
		_preset.add_item(text)
	_wizard.add_child(_preset)
	_label("New save slot", _wizard, 18)
	_slot_name = LineEdit.new()
	_slot_name.placeholder_text = "New slot name"
	_slot_name.text = "Adventure 1"
	_wizard.add_child(_slot_name)
	var wizard_buttons := HBoxContainer.new()
	wizard_buttons.add_theme_constant_override("separation", 12)
	_wizard.add_child(wizard_buttons)
	_finish = _button("Finish setup", wizard_buttons, _finish_setup)
	_button("Back", wizard_buttons, func(): back_requested.emit())
	_status = _label("", _wizard, 16)
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART

	_title_screen = VBoxContainer.new()
	_title_screen.add_theme_constant_override("separation", 14)
	content.add_child(_title_screen)
	_label("YOUR JOURNEY", _title_screen, 16).add_theme_color_override("font_color", Observatory.GOLD)
	_continue = _button("Continue", _title_screen, func(): continue_requested.emit())
	_button("New Game", _title_screen, func(): new_game_requested.emit())
	_button("Settings", _title_screen, func(): settings_requested.emit())
	_button("Save Slots", _title_screen, func(): slots_requested.emit())
	_button("Quit", _title_screen, func(): quit_requested.emit())
	var spacer := Control.new()
	spacer.size_flags_vertical = Control.SIZE_EXPAND_FILL
	content.add_child(spacer)
	_prompts = _label("Enter  Select     Esc  Back", content, 15)
	_prompts.add_theme_color_override("font_color", Color(Observatory.IVORY, 0.72))
	_update_prompts()
	var version := Label.new()
	var version_value := str(ProjectSettings.get_setting("application/config/version", "")).strip_edges()
	version.text = "v" + version_value.trim_prefix("v") if not version_value.is_empty() else ""
	version.visible = not version_value.is_empty()
	version.add_theme_font_size_override("font_size", 12)
	version.add_theme_color_override("font_color", Color(Observatory.IVORY, 0.45))
	version.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_RIGHT)
	version.position = Vector2(-180, -32)
	add_child(version)
	var title_buttons: Array[Button] = []
	for child in _title_screen.get_children():
		if child is Button: title_buttons.append(child)
	for i in title_buttons.size():
		title_buttons[i].focus_neighbor_top = title_buttons[(i - 1 + title_buttons.size()) % title_buttons.size()].get_path()
		title_buttons[i].focus_neighbor_bottom = title_buttons[(i + 1) % title_buttons.size()].get_path()
	_dialog = FileDialog.new()
	_dialog.access = FileDialog.ACCESS_FILESYSTEM
	_dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	_dialog.add_filter("*.nds,*.NDS", "Nintendo DS ROM")
	_dialog.file_selected.connect(_rom_picked)
	add_child(_dialog)
	_continue.disabled = not _continue_enabled
	_update_rom()
	show_title()


func _label(text: String, parent: Node, font_size: int) -> Label:
	var label := Label.new()
	label.text = text
	label.add_theme_font_size_override("font_size", font_size)
	parent.add_child(label)
	return label


func _button(text: String, parent: Node, action: Callable) -> Button:
	var button := Button.new()
	button.text = "    " + text
	button.alignment = HORIZONTAL_ALIGNMENT_LEFT
	button.custom_minimum_size = Vector2(400, 48)
	button.focus_mode = Control.FOCUS_ALL
	var focus_style := StyleBoxFlat.new()
	focus_style.bg_color = Color.TRANSPARENT
	focus_style.border_color = Color(Observatory.GOLD, 0.0)
	focus_style.border_width_bottom = 2
	button.add_theme_stylebox_override("focus", focus_style)
	button.focus_entered.connect(func():
		button.text = "◆  " + text
		if parent == _title_screen: _last_title_focus = button
		_focus_lift(button, true)
		_sound("cursor"))
	button.mouse_entered.connect(func(): if not button.disabled: button.grab_focus())
	button.focus_exited.connect(func(): button.text = "    " + text; _focus_lift(button, false))
	button.pressed.connect(func(): _sound("confirm"); action.call())
	parent.add_child(button)
	return button

func _focus_lift(button: Button, focused: bool) -> void:
	if button.has_meta("focus_tween"):
		var previous = button.get_meta("focus_tween")
		if previous is Tween: previous.kill()
	var target := Color.WHITE if focused else Color(0.78, 0.78, 0.78)
	var focus_style := button.get_theme_stylebox("focus") as StyleBoxFlat
	var line_color := Color(Observatory.GOLD, 1.0 if focused else 0.0)
	if _reduced_motion:
		button.modulate = target
		focus_style.border_color = line_color
	else:
		var tween := create_tween().set_parallel()
		button.set_meta("focus_tween", tween)
		tween.tween_property(button, "modulate", target, 0.14)
		tween.tween_property(focus_style, "border_color", line_color, 0.14)


# Display methods are called after adding the view to the scene tree.
func show_wizard() -> void:
	show()
	_title_screen.hide()
	_wizard.show()
	_animate_page(_wizard)
	_browse.grab_focus.call_deferred()


func show_title() -> void:
	show()
	_wizard.hide()
	_title_screen.show()
	_animate_page(_title_screen)
	if is_instance_valid(_last_title_focus) and not _last_title_focus.disabled:
		_last_title_focus.grab_focus.call_deferred()
	elif _continue.disabled:
		(_title_screen.get_child(2) as Button).grab_focus.call_deferred()
	else:
		_continue.grab_focus.call_deferred()

func _animate_page(page: Control) -> void:
	if _entry_tween: _entry_tween.kill()
	# Container-managed positions change with the viewport and visibility. Never
	# cache or animate them: doing so restores stale layout after Settings.
	page.modulate.a = 1.0 if _reduced_motion else 0.0
	if not _reduced_motion:
		_entry_tween = create_tween()
		_entry_tween.tween_property(page, "modulate:a", 1.0, 0.22)


# Call only with an accepted ROM; clear the path when selection is invalid.
func set_rom(path: String, title: String, icon: Texture2D) -> void:
	_rom_path = path
	_game_title.text = title if not title.is_empty() else "Dragon Quest IX"
	_art.texture = icon
	_update_rom()


func set_status(message: String, is_error: bool = false) -> void:
	_status.text = message
	_status.add_theme_color_override("font_color", Color(1.0, 0.55, 0.5) if is_error else Color(0.7, 0.85, 1.0))


func set_continue_enabled(enabled: bool) -> void:
	_continue_enabled = enabled
	if is_instance_valid(_continue):
		_continue.disabled = not enabled



func set_wizard_choices(language: String, preset: int) -> void:
	_language.select(maxi(0, LANGUAGES.find(language)))
	_preset.select(clampi(preset, 0, _preset.item_count - 1))


func _update_rom() -> void:
	_rom_label.text = _rom_path.get_file() if not _rom_path.is_empty() else "No ROM selected"
	_rom_label.tooltip_text = _rom_path
	_finish.disabled = _rom_path.is_empty()



func _pick_rom() -> void:
	if not _rom_path.is_empty():
		_dialog.current_dir = _rom_path.get_base_dir()
	_dialog.popup_centered_ratio(0.8)


func _rom_picked(path: String) -> void:
	# Invalidate the previous selection until the parent accepts this one.
	_rom_path = ""
	_update_rom()
	rom_selected.emit(path)


func _finish_setup() -> void:
	var slot_name := _slot_name.text.strip_edges()
	if slot_name.is_empty():
		set_status("Enter a name for the new save slot.", true)
		_slot_name.grab_focus()
		return
	wizard_completed.emit(LANGUAGES[_language.selected], _preset.selected, "", slot_name)

func _joy_connection_changed(device: int, connected: bool) -> void:
	if connected:
		_prompt_joypad = device
	elif _prompt_joypad == device:
		var pads := Input.get_connected_joypads()
		_prompt_joypad = pads[0] if not pads.is_empty() else -1
	if is_instance_valid(_prompts): _update_prompts()

static func _joy_button_name(index: int, device_name: String) -> String:
	var name := device_name.to_lower()
	var names: Array = []
	if "playstation" in name or "dualshock" in name or "dualsense" in name or "sony" in name:
		names = ["Cross", "Circle", "Square", "Triangle", "Share", "PS", "Options", "L3", "R3", "L1", "R1"]
	elif "nintendo" in name or "switch" in name or "joy-con" in name:
		names = ["B", "A", "Y", "X", "Minus", "Home", "Plus", "L3", "R3", "L", "R"]
	elif "xbox" in name or "xinput" in name or "x-box" in name:
		names = ["A", "B", "X", "Y", "Back", "Guide", "Start", "L3", "R3", "L", "R"]
	return names[index] if index >= 0 and index < names.size() else "Button %d" % (index + 1)

func _action_prompt(action: String, device: int) -> String:
	for binding in InputMap.action_get_events(action):
		if device >= 0 and binding is InputEventJoypadButton:
			return _joy_button_name(binding.button_index, Input.get_joy_name(device))
		if device < 0 and binding is InputEventKey:
			return binding.as_text().replace(" (Physical)", "")
	# Built-in UI actions also handle south/east buttons without InputMap entries.
	if device >= 0:
		return _joy_button_name(JOY_BUTTON_A if action == "ui_accept" else JOY_BUTTON_B, Input.get_joy_name(device))
	return "Enter" if action == "ui_accept" else "Escape"

func _update_prompts() -> void:
	_prompts.text = "%s  Select     %s  Back" % [_action_prompt("ui_accept", _prompt_joypad), _action_prompt("ui_cancel", _prompt_joypad)]

func _input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		_prompt_joypad = -1
		if is_instance_valid(_prompts): _update_prompts()
	elif (event is InputEventJoypadButton and event.pressed) or (event is InputEventJoypadMotion and absf(event.axis_value) > 0.5):
		_prompt_joypad = event.device
		if is_instance_valid(_prompts): _update_prompts()

func _unhandled_input(event: InputEvent) -> void:
	if not is_visible_in_tree() or _dialog.visible:
		return
	if event.is_action_pressed("ui_cancel"):
		_sound("cancel")
		if _wizard.visible:
			back_requested.emit()
		else:
			quit_requested.emit()
		get_viewport().set_input_as_handled()
