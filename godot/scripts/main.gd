extends Control

const Logic = preload("res://scripts/host_logic.gd")
const BUTTON_ACTIONS := Logic.BUTTON_ACTIONS
const BUTTON_BITS := Logic.BUTTON_BITS
const DebugBundle = preload("res://scripts/debug_bundle.gd")
const IssueReviewer = preload("res://scripts/issue_reviewer.gd")
const LANGS := ["en","fr","de","es","it"]
var settings: Dictionary
var machine: Node
var layout := 0
var _user_speed := 1.0          # speed chosen in the pause panel; fast-forward (hold Tab) overrides it
var _ff_held := false
var _toast: Label
var _toast_token := 0
var settings_panel: PanelContainer
var game_panel: Control
var top_rect: TextureRect
var bottom_rect: TextureRect
var top_frame: PanelContainer
var bottom_frame: PanelContainer
var status_label: Label
var pause_panel: PanelContainer
var pause_note: TextEdit
var rebinding_action := ""
var _chord_latched := false
var _touching := false
var _overlay_drag := false
var _overlay_resize := false
var _overlay_start := Vector2.ZERO
var _overlay_origin := Vector2.ZERO
var _overlay_size_origin := Vector2.ZERO
var _overlay_rect := Rect2(0.58, 0.56, 0.35, 0.35)
var _ui: Dictionary = {}
var _fps := 0.0
var _fps_frame := 0
var _fps_ms := 0
var issue_reviewer: PanelContainer
var replay_issue: Dictionary = {}
var replay_save_path := ""
var replay_started_ms := 0
var replay_complete := false
var _reviewer_was_paused := false
var _review_build_id := ""
# Engine A can appear on either physical screen (POWCNT1 screen swap).
# The producer supplies a mask and a texture from the same completed frame.
var gpu3d_screen_is_top := true
var gpu3d_rect: TextureRect
var _gpu3d_texture: Texture2D
var _gpu3d_mask: Texture2D
# A/B changes presentation only: keep the same GPU scene alive even while
# paused, so comparison never waits for another guest frame or a cache warmup.
var _gpu3d_compare_software := false
const GPU3D_STATUS_FRAMES := 120
var _gpu3d_stats: Dictionary = {}
var _gpu3d_valid_window := PackedByteArray()
var _gpu3d_valid_index := 0
var _gpu3d_valid_count := 0
var _gpu3d_frame_count := 0

func _ready() -> void:
	get_window().title = "DQ9-Decomp"
	# A clean command-line checkout has no editor-generated extension_list.cfg.
	if not ClassDB.class_exists("DqMachine"):
		GDExtensionManager.load_extension("res://dq9.gdextension")
	settings = Logic.load_settings("user://settings.cfg")
	Logic.apply_bindings("user://settings.cfg")
	layout = int(settings.layout); _overlay_rect = settings.overlay_rect
	_parse_args()
	_build_ui()
	get_window().size = Vector2i(1280,800) * int(settings.window_scale)
	if not settings.rom_path.is_empty() and _ui.get("autostart", false): _start_game()
	else: _show_settings()

func _parse_args() -> void:
	var args := OS.get_cmdline_user_args()
	if settings.rom_path.is_empty():
		settings.rom_path = OS.get_environment("DQ9_ROM")
	for i in args.size():
		match args[i]:
			"--rom": if i+1 < args.size(): settings.rom_path = args[i+1]
			"--save": if i+1 < args.size(): settings.save_path = args[i+1]
			"--language": if i+1 < args.size(): settings.language = args[i+1].to_lower()
			"--backup-type": if i+1 < args.size(): settings.backup_type = args[i+1].to_lower()
			"--autostart": _ui.autostart = true

func _build_ui() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	var root := VBoxContainer.new(); root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT); root.add_theme_constant_override("separation", 8); add_child(root)
	game_panel = Control.new(); game_panel.size_flags_vertical = Control.SIZE_EXPAND_FILL; game_panel.clip_contents = true; root.add_child(game_panel)
	top_frame = _screen_frame("Top screen"); game_panel.add_child(top_frame)
	bottom_frame = _screen_frame("Touch screen"); game_panel.add_child(bottom_frame)
	top_rect = top_frame.find_child("Texture", true, false) as TextureRect
	bottom_rect = bottom_frame.find_child("Texture", true, false) as TextureRect
	bottom_rect.gui_input.connect(_on_touch_input)
	status_label = Label.new(); status_label.position = Vector2(10, 10); status_label.add_theme_font_size_override("font_size", 16)
	status_label.add_theme_color_override("font_outline_color", Color(0, 0, 0, 0.9)); status_label.add_theme_constant_override("outline_size", 5) # readable over bright scenes
	game_panel.add_child(status_label)
	_toast = Label.new(); _toast.add_theme_font_size_override("font_size", 18); _toast.hide()
	_toast.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_LEFT); _toast.position = Vector2(12, -40)
	var toast_style := StyleBoxFlat.new(); toast_style.bg_color = Color(0.05, 0.08, 0.13, 0.92); toast_style.border_color = Color(0.3, 0.65, 0.95); toast_style.set_border_width_all(1); toast_style.set_corner_radius_all(4); toast_style.set_content_margin_all(6)
	_toast.add_theme_stylebox_override("normal", toast_style)
	game_panel.add_child(_toast)
	var bar_panel := PanelContainer.new(); root.add_child(bar_panel)
	var bar_style := StyleBoxFlat.new(); bar_style.bg_color = Color(0.055, 0.07, 0.1); bar_style.set_content_margin_all(4)
	bar_style.border_color = Color(0.3, 0.65, 0.95, 0.5); bar_style.border_width_top = 1
	bar_panel.add_theme_stylebox_override("panel", bar_style)
	var bar := HBoxContainer.new(); bar.add_theme_constant_override("separation", 8); bar_panel.add_child(bar)
	var menu := Button.new(); menu.text = "Start / Settings"; menu.tooltip_text = "ROM, language, display, audio and input settings"; menu.pressed.connect(_show_settings); bar.add_child(menu)
	var layout_box := OptionButton.new()
	for name in Logic.LAYOUTS: layout_box.add_item(name)
	layout_box.selected = layout
	layout_box.item_selected.connect(_set_layout)
	bar.add_child(layout_box)
	var filter := CheckButton.new(); filter.text = "Nearest"; filter.button_pressed = settings.nearest; filter.toggled.connect(func(v): settings.nearest=v; _apply_filter()); bar.add_child(filter)
	var integer := CheckButton.new(); integer.text = "Integer scale"; integer.button_pressed = settings.integer_scale; integer.toggled.connect(func(v): settings.integer_scale=v; _layout_screens()); bar.add_child(integer)
	var debug := Button.new(); debug.text = "Debug (F10)"; debug.tooltip_text = "Pause and open the debug panel (also Escape or Select+Start)"; debug.pressed.connect(_toggle_pause); bar.add_child(debug)
	var hint := Label.new(); hint.text = "Tab: fast-forward   F12: screenshot   F11: fullscreen"; hint.add_theme_color_override("font_color", Color(0.55, 0.65, 0.78)); hint.size_flags_horizontal = Control.SIZE_EXPAND_FILL; hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT; hint.add_theme_font_size_override("font_size", 13); bar.add_child(hint)
	var compare := Button.new(); compare.text = "3D A/B (F6)"; compare.tooltip_text = "With GPU hi-res selected, compare it with software instantly (also while paused). 2D stays unchanged."
	compare.disabled = int(settings.renderer_gpu3d) == 0; compare.pressed.connect(_toggle_gpu3d_compare); bar.add_child(compare); _ui.gpu_compare = compare
	var settings_overlay := Control.new()
	settings_overlay.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	settings_overlay.mouse_filter = Control.MOUSE_FILTER_IGNORE
	settings_overlay.z_index = 10
	add_child(settings_overlay)
	settings_panel = _make_settings()
	settings_overlay.add_child(settings_panel)
	settings_panel.hide()
	var pause_overlay := Control.new()
	pause_overlay.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	pause_overlay.mouse_filter = Control.MOUSE_FILTER_IGNORE
	pause_overlay.z_index = 10
	add_child(pause_overlay)
	pause_panel = _make_pause()
	pause_overlay.add_child(pause_panel)
	pause_panel.hide()
	var reviewer_overlay := Control.new()
	reviewer_overlay.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	reviewer_overlay.mouse_filter = Control.MOUSE_FILTER_IGNORE
	reviewer_overlay.z_index = 20
	add_child(reviewer_overlay)
	issue_reviewer = IssueReviewer.new()
	reviewer_overlay.add_child(issue_reviewer)
	issue_reviewer.hide()
	issue_reviewer.replay_requested.connect(_start_issue_replay)
	issue_reviewer.compare_requested.connect(_compare_issue)
	issue_reviewer.play_requested.connect(_play_from_issue)
	issue_reviewer.cancel_requested.connect(_cancel_issue_replay)
	issue_reviewer.return_requested.connect(_return_to_normal_game)
	issue_reviewer.closed.connect(_close_issue_reviewer)
	issue_reviewer.preferences_changed.connect(func(value): settings.issue_show_auto = value; _persist())
	get_viewport().size_changed.connect(_layout_screens)
	get_viewport().size_changed.connect(_fit_panels)
	_fit_panels()
	game_panel.resized.connect(_layout_screens)
	_layout_screens()

func _screen_frame(title: String) -> PanelContainer:
	var frame := PanelContainer.new(); frame.clip_contents = true; frame.tooltip_text = title
	frame.add_theme_stylebox_override("panel", StyleBoxEmpty.new())
	var image := TextureRect.new(); image.name="Texture"; image.expand_mode=TextureRect.EXPAND_IGNORE_SIZE; image.stretch_mode=TextureRect.STRETCH_SCALE; image.texture_filter=CanvasItem.TEXTURE_FILTER_NEAREST; frame.add_child(image)
	return frame

func _make_settings() -> PanelContainer:
	var panel := PanelContainer.new()
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.055, 0.07, 0.1, 1.0)
	style.border_color = Color(0.3, 0.65, 0.95, 1.0)
	style.set_border_width_all(2)
	style.set_corner_radius_all(10)
	style.set_content_margin_all(14)
	panel.add_theme_stylebox_override("panel", style)
	# Title and the Start/Close row stay pinned; the settings sections scroll between them.
	var outer:=VBoxContainer.new(); outer.add_theme_constant_override("separation", 6); panel.add_child(outer)
	var title:=Label.new(); title.text="DQ9-Decomp — Start / Settings"; title.add_theme_font_size_override("font_size",26); outer.add_child(title)
	var subtitle:=Label.new(); subtitle.text="Choose your ROM and language, then press Start."; subtitle.add_theme_color_override("font_color",Color(0.62,0.7,0.8)); outer.add_child(subtitle)
	var scroll:=ScrollContainer.new(); scroll.horizontal_scroll_mode=ScrollContainer.SCROLL_MODE_DISABLED; scroll.custom_minimum_size=Vector2(0,120); scroll.size_flags_vertical=Control.SIZE_EXPAND_FILL; outer.add_child(scroll)
	var col:=VBoxContainer.new(); col.add_theme_constant_override("separation", 6); col.size_flags_horizontal=Control.SIZE_EXPAND_FILL; scroll.add_child(col)
	_section(col,"Game")
	var pathrow:=HBoxContainer.new(); col.add_child(pathrow)
	var rom:=LineEdit.new(); rom.placeholder_text="Nintendo DS .nds ROM path"; rom.text=settings.rom_path; rom.size_flags_horizontal=Control.SIZE_EXPAND_FILL; pathrow.add_child(rom); _ui.rom=rom
	var browse:=Button.new(); browse.text="Browse…"; browse.pressed.connect(func(): _pick_path(rom,"*.nds,*.rom")); pathrow.add_child(browse)
	var rom_status:=Label.new(); col.add_child(rom_status); _ui.rom_status=rom_status
	rom.text_changed.connect(func(_t): _update_rom_status())
	_update_rom_status()
	var saverow:=HBoxContainer.new(); col.add_child(saverow); var save:=LineEdit.new(); save.text=settings.save_path; save.placeholder_text="Save path"; save.size_flags_horizontal=Control.SIZE_EXPAND_FILL; saverow.add_child(save); _ui.save=save
	var save_browse := Button.new(); save_browse.text="Browse…"; save_browse.pressed.connect(func(): _pick_path(save,"*.sav",true)); saverow.add_child(save_browse)
	var lang_row:=HBoxContainer.new(); col.add_child(lang_row)
	var lang_label:=Label.new(); lang_label.text="Language"; lang_label.custom_minimum_size.x=110; lang_row.add_child(lang_label)
	var lang:=OptionButton.new(); lang.size_flags_horizontal=Control.SIZE_EXPAND_FILL
	for text in ["English","Français","Deutsch","Español","Italiano"]: lang.add_item(text)
	lang.select(maxi(0,LANGS.find(settings.language)))
	lang_row.add_child(lang)
	_ui.lang=lang
	var chip_row:=HBoxContainer.new(); col.add_child(chip_row)
	var chip_label:=Label.new(); chip_label.text="Save chip"; chip_label.custom_minimum_size.x=110; chip_row.add_child(chip_label)
	var chip:=OptionButton.new(); chip.size_flags_horizontal=Control.SIZE_EXPAND_FILL
	for text in Logic.BACKUP_LABELS: chip.add_item(text)
	chip.select(maxi(0,Logic.BACKUP_TYPES.find(settings.backup_type)))
	chip.tooltip_text="The cartridge's backup memory type. Auto uses the build's setting or the save file size. If the title screen says the quick save log is corrupted, choose EEPROM 64 KB."
	chip_row.add_child(chip)
	_ui.chip=chip
	_section(col,"Display")
	var options:=HBoxContainer.new(); col.add_child(options)
	var layoutbox:=OptionButton.new()
	for name in Logic.LAYOUTS: layoutbox.add_item(name)
	layoutbox.selected=layout
	layoutbox.item_selected.connect(_set_layout)
	options.add_child(layoutbox)
	var nearest:=CheckButton.new(); nearest.text="Nearest filter"; nearest.button_pressed=settings.nearest; nearest.toggled.connect(func(v):settings.nearest=v;_apply_filter()); options.add_child(nearest)
	var integer:=CheckButton.new(); integer.text="Integer scaling"; integer.button_pressed=settings.integer_scale; integer.toggled.connect(func(v):settings.integer_scale=v;_layout_screens()); options.add_child(integer)
	var scale_row:=HBoxContainer.new(); col.add_child(scale_row); var scalelabel:=Label.new(); scalelabel.text="Window scale"; scale_row.add_child(scalelabel)
	var scale:=OptionButton.new()
	for n in [1,2,3]: scale.add_item("%dx"%n)
	scale.select(clampi(settings.window_scale-1,0,2))
	scale.item_selected.connect(func(i): settings.window_scale=i+1; get_window().size=Vector2i(1280,800)*(i+1))
	scale_row.add_child(scale)
	var renderer_row := HBoxContainer.new(); col.add_child(renderer_row)
	var renderer_label := Label.new(); renderer_label.text = "3D renderer"; renderer_row.add_child(renderer_label)
	var renderer := OptionButton.new()
	renderer.add_item("Software (exact)")
	renderer.add_item("GPU hi-res (experimental)")
	renderer.select(int(settings.renderer_gpu3d))
	renderer.item_selected.connect(func(i): settings.renderer_gpu3d=i; _apply_gpu3d_settings(); _persist())
	renderer_row.add_child(renderer)
	_ui.gpu_renderer = renderer
	var gpu_scale_label := Label.new(); gpu_scale_label.text = "GPU scale"; renderer_row.add_child(gpu_scale_label)
	var gpu_scale := OptionButton.new()
	for n in range(2, 7): gpu_scale.add_item("%dx" % n)
	gpu_scale.select(int(settings.gpu3d_scale)-2)
	gpu_scale.disabled = int(settings.renderer_gpu3d) == 0
	gpu_scale.item_selected.connect(func(i): settings.gpu3d_scale=i+2; _apply_gpu3d_settings(); _persist())
	renderer_row.add_child(gpu_scale)
	_ui.gpu_scale = gpu_scale
	_section(col,"Audio")
	var vol_row:=HBoxContainer.new(); col.add_child(vol_row)
	var vol_label:=Label.new(); vol_label.text="Volume"; vol_label.custom_minimum_size.x=110; vol_row.add_child(vol_label)
	var vol:=HSlider.new(); vol.min_value=0; vol.max_value=1; vol.step=0.01; vol.value=settings.volume; vol.size_flags_horizontal=Control.SIZE_EXPAND_FILL; vol.size_flags_vertical=Control.SIZE_SHRINK_CENTER; vol_row.add_child(vol)
	var vol_value:=Label.new(); vol_value.custom_minimum_size.x=48; vol_value.horizontal_alignment=HORIZONTAL_ALIGNMENT_RIGHT; vol_value.text="%d%%"%roundi(float(settings.volume)*100.0); vol_row.add_child(vol_value)
	vol.value_changed.connect(func(v):settings.volume=v; vol_value.text="%d%%"%roundi(v*100.0); if machine: machine.get_audio_player().volume_linear=v)
	_section(col,"Input")
	var rebind_title:=Label.new(); rebind_title.text="Select a button, then press a key or gamepad control to rebind it."; col.add_child(rebind_title); _ui.rebind_title=rebind_title
	var grid:=GridContainer.new(); grid.columns=3; grid.add_theme_constant_override("h_separation",4); grid.add_theme_constant_override("v_separation",4); col.add_child(grid)
	_ui.bind_buttons = {}
	for action in BUTTON_ACTIONS:
		var b:=Button.new(); b.size_flags_horizontal=Control.SIZE_EXPAND_FILL; b.pressed.connect(func():rebinding_action=action; rebind_title.text="Press a key or gamepad control for %s…"%action.trim_prefix("ds_").capitalize()); grid.add_child(b)
		_ui.bind_buttons[action] = b
	_refresh_binding_buttons()
	var reset:=Button.new(); reset.text="Reset bindings"; reset.pressed.connect(func():Logic.reset_bindings(); _persist(); _refresh_binding_buttons()); col.add_child(reset)
	var error_label := Label.new(); error_label.autowrap_mode=TextServer.AUTOWRAP_WORD_SMART; error_label.add_theme_color_override("font_color",Color(1,0.45,0.4)); outer.add_child(error_label); _ui.error=error_label
	var buttons:=HBoxContainer.new(); buttons.add_theme_constant_override("separation",8); outer.add_child(buttons)
	var start:=Button.new(); start.text="Start"; start.custom_minimum_size=Vector2(220,52); start.size_flags_horizontal=Control.SIZE_EXPAND_FILL;
	var start_style:=StyleBoxFlat.new(); start_style.bg_color=Color(0.13,0.42,0.3); start_style.set_corner_radius_all(6); start_style.set_content_margin_all(8)
	start.add_theme_stylebox_override("normal",start_style)
	var start_hover:=start_style.duplicate() as StyleBoxFlat; start_hover.bg_color=Color(0.17,0.52,0.37); start.add_theme_stylebox_override("hover",start_hover)
	start.add_theme_font_size_override("font_size",20)
	start.pressed.connect(func():settings.rom_path=rom.text;settings.save_path=save.text;settings.language=LANGS[lang.selected];settings.backup_type=Logic.BACKUP_TYPES[chip.selected];_persist();_start_game()); buttons.add_child(start)
	var close:=Button.new(); close.text="Close"; close.pressed.connect(_hide_settings); buttons.add_child(close)
	var reviewer := Button.new(); reviewer.text = "Issue reviewer… (F7)"; reviewer.pressed.connect(_open_issue_reviewer); buttons.add_child(reviewer)
	return panel

func _section(parent: Control, text: String) -> void:
	var label := Label.new(); label.text = text.to_upper()
	label.add_theme_font_size_override("font_size", 12)
	label.add_theme_color_override("font_color", Color(0.45, 0.7, 0.95))
	parent.add_child(label)

# [message, ok] for the ROM path currently typed in the settings panel.
func _rom_status(path: String) -> Array:
	if path.is_empty(): return ["No ROM selected.", false]
	if not FileAccess.file_exists(path): return ["ROM not found at this path.", false]
	var file := FileAccess.open(path, FileAccess.READ)
	var megabytes := int(file.get_length() / 1048576) if file else 0
	return ["ROM found (%d MB)." % megabytes, true]

func _update_rom_status() -> void:
	var label: Label = _ui.get("rom_status")
	var rom: LineEdit = _ui.get("rom")
	if label == null or rom == null: return
	var status := _rom_status(rom.text)
	label.text = status[0]
	label.add_theme_color_override("font_color", Color(0.5, 0.85, 0.6) if status[1] else Color(1.0, 0.7, 0.4))

func _binding_text(action: String) -> String:
	var key := ""
	var pad := ""
	for event in InputMap.action_get_events(action):
		if event is InputEventKey and key.is_empty(): key = event.as_text_physical_keycode() if event.physical_keycode != 0 else event.as_text_keycode()
		elif event is InputEventJoypadButton and pad.is_empty(): pad = "Pad %d" % event.button_index
		elif event is InputEventJoypadMotion and pad.is_empty(): pad = "Axis %d%s" % [event.axis, "-" if event.axis_value < 0 else "+"]
	var parts: Array[String] = []
	if not key.is_empty(): parts.append(key)
	if not pad.is_empty(): parts.append(pad)
	return "%s  [%s]" % [action.trim_prefix("ds_").capitalize(), " · ".join(parts) if not parts.is_empty() else "unbound"]

func _refresh_binding_buttons() -> void:
	var buttons: Dictionary = _ui.get("bind_buttons", {})
	for action in buttons: (buttons[action] as Button).text = _binding_text(action)
	var title: Label = _ui.get("rebind_title")
	if title: title.text = "Select a button, then press a key or gamepad control to rebind it."

func _make_pause() -> PanelContainer:
	var p:=PanelContainer.new()
	var pstyle := StyleBoxFlat.new()
	pstyle.bg_color = Color(0.055, 0.07, 0.1, 1.0)
	pstyle.border_color = Color(0.3, 0.65, 0.95, 1.0)
	pstyle.set_border_width_all(2)
	pstyle.set_corner_radius_all(10)
	pstyle.set_content_margin_all(14)
	p.add_theme_stylebox_override("panel", pstyle)
	var scroll:=ScrollContainer.new(); scroll.horizontal_scroll_mode=ScrollContainer.SCROLL_MODE_DISABLED; p.add_child(scroll)
	var c:=VBoxContainer.new(); c.add_theme_constant_override("separation",6); c.size_flags_horizontal=Control.SIZE_EXPAND_FILL; scroll.add_child(c)
	var title:=Label.new(); title.text="Paused / Debug"; title.add_theme_font_size_override("font_size",26); c.add_child(title)
	var pause:=Button.new(); pause.text="Resume / Pause"; pause.pressed.connect(func():machine.set_paused(not machine.is_paused())); c.add_child(pause)
	var step:=Button.new(); step.text="Step one frame"; step.pressed.connect(func():machine.set_paused(true);machine.step_once()); c.add_child(step)
	var speeds:=OptionButton.new()
	for label in ["0.25×","0.5×","1×","2×","Max"]: speeds.add_item(label)
	speeds.item_selected.connect(func(i): _user_speed = [0.25,0.5,1.0,2.0,0.0][i]; machine.set_speed(_user_speed))
	speeds.select(2)
	c.add_child(speeds)
	var stats:=CheckButton.new(); stats.text="Show statistics"; stats.button_pressed=true; stats.toggled.connect(func(v):status_label.visible=v); c.add_child(stats)
	var shot:=Button.new(); shot.text="Save screenshot"; shot.pressed.connect(_save_screenshot); c.add_child(shot)
	pause_note=TextEdit.new(); pause_note.placeholder_text="Quick note"; pause_note.custom_minimum_size=Vector2(0,110); c.add_child(pause_note)
	var note:=Button.new(); note.text="Save note + screenshot bundle"; note.pressed.connect(_save_bundle); c.add_child(note)
	var reviewer := Button.new(); reviewer.text = "Issue reviewer…"; reviewer.pressed.connect(_open_issue_reviewer); c.add_child(reviewer)
	var close:=Button.new(); close.text="Close game"; close.pressed.connect(_stop_game); c.add_child(close)
	return p

func _pick_path(line: LineEdit, filters: String, save_file := false) -> void:
	var dialog:=FileDialog.new(); dialog.file_mode=FileDialog.FILE_MODE_SAVE_FILE if save_file else FileDialog.FILE_MODE_OPEN_FILE; dialog.access=FileDialog.ACCESS_FILESYSTEM; dialog.add_filter(filters); add_child(dialog)
	if not line.text.is_empty():
		var path := ProjectSettings.globalize_path(line.text)
		dialog.current_dir = path.get_base_dir()
		dialog.current_file = path.get_file()
	dialog.file_selected.connect(func(path):line.text=path;dialog.queue_free()); dialog.canceled.connect(dialog.queue_free); dialog.popup_centered_ratio(0.8)

func _start_game() -> void:
	_ui.error.text = ""
	if settings.rom_path.is_empty():
		_ui.error.text = "Choose a Nintendo DS ROM before starting."
		_show_settings()
		return
	_release_machine(false) # flush the old save and stop its worker before the replacement loads from disk
	issue_reviewer.set_replay_state("", false)
	if not ClassDB.class_exists("DqMachine"):
		_ui.error.text = "Native extension unavailable. Build the Godot extension before starting a game."
		_show_settings()
		return
	machine=ClassDB.instantiate("DqMachine")
	add_child(machine)
	_user_speed = 1.0; _ff_held = false
	_apply_gpu3d_settings()
	if machine.has_method("set_rtc_epoch"):
		var timezone := Time.get_time_zone_from_system()
		machine.set_rtc_epoch(int(Time.get_unix_time_from_system()), int(timezone.get("bias", 0)))
	var actual_save_path := ProjectSettings.globalize_path(settings.save_path)
	if machine.has_method("set_backup_type"): machine.set_backup_type(settings.backup_type)
	var result: String = machine.load_game(settings.rom_path, actual_save_path, Logic.firmware_language(settings.language))
	if not result.is_empty():
		push_error(result)
		_ui.error.text = result
		_release_machine(false)
		_show_settings()
		return
	machine.start()
	_on_machine_frame(0)
	if machine.has_signal("frame_ready"): machine.frame_ready.connect(_on_machine_frame)
	machine.get_audio_player().volume_linear=settings.volume
	_hide_settings()

func _on_machine_frame(_frame: int) -> void:
	if machine == null or not is_instance_valid(machine): return
	top_rect.texture=machine.get_top_texture()
	bottom_rect.texture=machine.get_bottom_texture()
	if int(settings.renderer_gpu3d) != 0 and machine.has_method("get_gpu3d_texture") and machine.has_method("get_gpu3d_mask_texture"):
		var screen_is_top := gpu3d_screen_is_top
		if machine.has_method("get_gpu3d_screen_is_top"):
			screen_is_top = bool(machine.get_gpu3d_screen_is_top())
		set_gpu3d_frame(machine.get_gpu3d_texture(), machine.get_gpu3d_mask_texture(), screen_is_top)
		if machine.has_method("get_gpu3d_stats"):
			_gpu3d_stats = machine.get_gpu3d_stats()
			_observe_gpu3d_frame()

# Integration point also used by synthetic producers. Null textures select the
# software result for this frame (unsupported GPU effects, shadows, etc.).
func set_gpu3d_frame(texture: Texture2D, mask: Texture2D, screen_is_top := true) -> void:
	_gpu3d_texture = texture
	_gpu3d_mask = mask
	gpu3d_screen_is_top = screen_is_top
	_update_gpu3d_composite()

func _apply_gpu3d_settings() -> void:
	var enabled := int(settings.renderer_gpu3d) != 0
	if _ui.has("gpu_scale"): _ui.gpu_scale.disabled = not enabled
	if _ui.has("gpu_compare"): _ui.gpu_compare.disabled = not enabled
	if not enabled:
		_gpu3d_compare_software = false
		_clear_gpu3d_status()
	if machine:
		if machine.has_method("set_gpu3d_scale"): machine.set_gpu3d_scale(int(settings.gpu3d_scale))
		if machine.has_method("set_gpu3d_enabled"): machine.set_gpu3d_enabled(enabled)
		# Disabling releases the producer's viewport. Re-enabling must not
		# resurrect its cached texture/mask while the new epoch warms up.
		if machine.has_method("get_gpu3d_texture") and machine.has_method("get_gpu3d_mask_texture"):
			_gpu3d_texture = machine.get_gpu3d_texture()
			_gpu3d_mask = machine.get_gpu3d_mask_texture()
	_update_gpu3d_composite()

func _update_gpu3d_composite() -> void:
	if int(settings.renderer_gpu3d) == 0 or _gpu3d_texture == null or _gpu3d_mask == null:
		if is_instance_valid(gpu3d_rect):
			gpu3d_rect.free()
			gpu3d_rect = null
		return
	var screen := top_rect if gpu3d_screen_is_top else bottom_rect
	if not is_instance_valid(screen): return
	if not is_instance_valid(gpu3d_rect):
		gpu3d_rect = TextureRect.new()
		gpu3d_rect.name = "Gpu3D"
		gpu3d_rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
		gpu3d_rect.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
		gpu3d_rect.stretch_mode = TextureRect.STRETCH_SCALE
		gpu3d_rect.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
		var composite := ShaderMaterial.new()
		composite.shader = load("res://shaders/composite_3d.gdshader")
		gpu3d_rect.material = composite
		screen.add_child(gpu3d_rect)
	elif gpu3d_rect.get_parent() != screen:
		gpu3d_rect.reparent(screen, false)
	# Parenting inside the physical screen keeps the mask aligned for every
	# layout, and the bottom overlay above the top GPU layer in merged mode.
	gpu3d_rect.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	gpu3d_rect.texture = _gpu3d_texture
	(gpu3d_rect.material as ShaderMaterial).set_shader_parameter("three_d_mask", _gpu3d_mask)
	gpu3d_rect.visible = not _gpu3d_compare_software

func _toggle_gpu3d_compare() -> void:
	if int(settings.renderer_gpu3d) == 0: return
	_gpu3d_compare_software = not _gpu3d_compare_software
	_update_gpu3d_composite()

func _clear_gpu3d_status() -> void:
	_gpu3d_stats = {}
	_gpu3d_valid_window.fill(0)
	_gpu3d_valid_index = 0
	_gpu3d_valid_count = 0
	_gpu3d_frame_count = 0

func _observe_gpu3d_frame() -> void:
	if _gpu3d_valid_window.is_empty(): _gpu3d_valid_window.resize(GPU3D_STATUS_FRAMES)
	_gpu3d_valid_count -= int(_gpu3d_valid_window[_gpu3d_valid_index])
	var valid := int(bool(_gpu3d_stats.get("valid", false)))
	_gpu3d_valid_window[_gpu3d_valid_index] = valid
	_gpu3d_valid_count += valid
	_gpu3d_valid_index = (_gpu3d_valid_index + 1) % GPU3D_STATUS_FRAMES
	_gpu3d_frame_count = mini(_gpu3d_frame_count + 1, GPU3D_STATUS_FRAMES)

func _gpu3d_status_text() -> String:
	if int(settings.renderer_gpu3d) == 0: return "GPU3D: off"
	var valid_percent := roundi(100.0 * _gpu3d_valid_count / maxi(1, _gpu3d_frame_count))
	var pixels := roundi(100.0 * float(_gpu3d_stats.get("gpu_pixel_share", 0.0)))
	var valid := bool(_gpu3d_stats.get("valid", false))
	var has_pixels := int(_gpu3d_stats.get("gpu_pixels", 0)) > 0
	var screen := "top" if gpu3d_screen_is_top else "bottom"
	var state := "active on %s (F6: A/B)" % screen if valid and has_pixels else "fallback: %s" % _gpu3d_stats.get("fallback", "warmup")
	if valid and not has_pixels: state = "no 3D pixels"
	if _gpu3d_compare_software: state = "software A/B (F6: GPU)"
	return "GPU3D: %dx valid %d%%   3D pixels %d%%   %s" % [int(settings.gpu3d_scale), valid_percent, pixels, state]

func _release_machine(drain_audio: bool) -> void:
	var closing = machine
	set_gpu3d_frame(null, null)
	_gpu3d_compare_software = false
	_clear_gpu3d_status()
	if closing == null or not is_instance_valid(closing): return
	machine = null
	if closing.has_signal("frame_ready"):
		var callback := Callable(self, "_on_machine_frame")
		if closing.is_connected("frame_ready", callback): closing.disconnect("frame_ready", callback)
	if closing.has_method("flush_save"): closing.flush_save()
	var audio_player: AudioStreamPlayer = closing.get_audio_player() if closing.has_method("get_audio_player") else null
	if audio_player: audio_player.stop()
	if closing.has_method("stop"): closing.stop()
	closing.free()
	_cleanup_replay_save()
	if drain_audio: OS.delay_msec(500)

func _stop_game() -> void:
	_release_machine(true)
	pause_panel.hide(); _show_settings()

func _open_issue_reviewer() -> void:
	if issue_reviewer.visible:
		_close_issue_reviewer()
		return
	_reviewer_was_paused = machine.is_paused() if machine else false
	if machine and not machine.get_stats().get("replay_active", false): machine.set_paused(true)
	pause_panel.hide(); settings_panel.hide()
	issue_reviewer.open(bool(settings.issue_show_auto))

func _close_issue_reviewer() -> void:
	if machine and machine.get_stats().get("replay_active", false):
		issue_reviewer.show_message("Cancel the replay before closing the reviewer.")
		return
	issue_reviewer.hide()
	if machine:
		# A replay stays paused until the user explicitly chooses Play from here.
		machine.set_paused(_reviewer_was_paused)
	else: settings_panel.show()

func _start_issue_replay(issue: Dictionary, record_hashes := false) -> void:
	if issue.is_empty(): return
	var path: String = issue.path
	var save_source := path.path_join("session_start.sav")
	var script_path := path.path_join("session_input.txt")
	var environment: Dictionary = issue.env
	var rom_path: String = str(environment.get("rom_path", settings.rom_path))
	if not FileAccess.file_exists(rom_path): rom_path = settings.rom_path
	if not FileAccess.file_exists(rom_path) or not FileAccess.file_exists(save_source) or not FileAccess.file_exists(script_path) or int(issue.frame) < 0:
		issue_reviewer.show_message("Replay needs a local ROM, session_start.sav, session_input.txt and a recorded frame. Choose the ROM in Start / Settings if its original path has moved.")
		return
	if not ClassDB.class_exists("DqMachine"):
		issue_reviewer.show_message("Build the native Godot extension before replaying.")
		return
	var temporary_dir := OS.get_cache_dir().path_join("dq9-decomp/issue-replay-%d-%d" % [OS.get_process_id(), Time.get_ticks_usec()])
	if DirAccess.make_dir_recursive_absolute(temporary_dir) != OK:
		issue_reviewer.show_message("Cannot create temporary replay save folder.")
		return
	var temporary_save := temporary_dir.path_join("replay.sav")
	if DirAccess.copy_absolute(save_source, temporary_save) != OK:
		DirAccess.remove_absolute(temporary_dir)
		issue_reviewer.show_message("Cannot copy the bundle's launch save.")
		return
	_release_machine(false) # flush normal save before replacing the session
	replay_save_path = temporary_save
	replay_issue = issue.duplicate(true); replay_complete = false
	machine = ClassDB.instantiate("DqMachine"); add_child(machine)
	# Software screenshots are the exact reference even when normal play uses GPU 3D.
	machine.set_gpu3d_enabled(false)
	var metadata := IssueReviewer.Store.fields(issue.comment)
	machine.set_rtc_epoch(int(environment.get("rtc_epoch", metadata.get("rtc_epoch", 0))), int(environment.get("rtc_timezone_offset_minutes", metadata.get("rtc_timezone_offset_minutes", 0))))
	var error: String = machine.load_game(rom_path, replay_save_path, Logic.firmware_language(issue.language))
	if error.is_empty(): error = machine.set_input_script(FileAccess.get_file_as_string(script_path).split("\n"))
	if not error.is_empty():
		_release_machine(false)
		issue_reviewer.show_message(error)
		return
	machine.frame_ready.connect(_on_machine_frame)
	machine.replay_finished.connect(_on_issue_replay_finished)
	machine.error.connect(_on_issue_replay_error, CONNECT_DEFERRED)
	machine.get_audio_player().volume_linear = settings.volume
	replay_started_ms = Time.get_ticks_msec()
	issue_reviewer.set_replay_state(path, true)
	var status_error: String = issue_reviewer.store.set_status(path, "testing", "Started deterministic replay")
	issue_reviewer.refresh()
	if not status_error.is_empty(): issue_reviewer.show_message(status_error)
	error = machine.start_replay(int(issue.frame), record_hashes)
	if not error.is_empty(): _on_issue_replay_error(error)

func _on_issue_replay_finished(frame: int) -> void:
	if replay_issue.is_empty(): return
	replay_complete = true
	_reviewer_was_paused = true
	issue_reviewer.set_replay_state(replay_issue.path, false, true)
	var error: String = issue_reviewer.store.record_test(replay_issue.path, frame, null, _review_build())
	issue_reviewer.refresh()
	if not error.is_empty(): issue_reviewer.show_message(error)

func _compare_issue(issue: Dictionary) -> void:
	if not machine or not replay_complete or replay_issue.is_empty() or issue.path != replay_issue.path or _current_frame() != int(issue.frame) or not machine.is_paused():
		issue_reviewer.show_message("Replay to this issue and stay paused at its recorded frame before comparing.")
		return
	var both := Image.create(256, 384, false, Image.FORMAT_RGBA8)
	both.blit_rect(machine.get_top_image(), Rect2i(0, 0, 256, 192), Vector2i.ZERO)
	both.blit_rect(machine.get_bottom_image(), Rect2i(0, 0, 256, 192), Vector2i(0, 192))
	var pixels: int = issue_reviewer.show_comparison(both)
	if pixels >= 0:
		var error: String = issue_reviewer.store.record_test(issue.path, int(issue.frame), pixels, _review_build())
		issue_reviewer.refresh()
		if not error.is_empty(): issue_reviewer.show_message(error)

func _play_from_issue() -> void:
	if not machine or not replay_complete: return
	machine.set_buttons(0); machine.set_touch(0, 0, false)
	machine.set_speed(1.0); machine.set_paused(false)
	issue_reviewer.hide()
	# The return action remains available when the reviewer is reopened.

func _cancel_issue_replay() -> void:
	if replay_issue.is_empty(): return
	var path: String = replay_issue.path
	if machine: machine.cancel_replay()
	_release_machine(false)
	issue_reviewer.set_replay_state(path, false)
	_offer_normal_game()

func _on_issue_replay_error(error: String) -> void:
	var path: String = replay_issue.get("path", "")
	_release_machine(false)
	issue_reviewer.set_replay_state(path, false)
	issue_reviewer.show_message(error)
	_offer_normal_game()

func _offer_normal_game() -> void:
	var dialog := ConfirmationDialog.new(); dialog.title = "Issue replay stopped"
	dialog.dialog_text = "Return to the normal game? Your normal save was flushed before replay and was not used by the replay."
	dialog.ok_button_text = "Return to normal game"; dialog.cancel_button_text = "Keep reviewing"
	add_child(dialog)
	dialog.confirmed.connect(func(): dialog.queue_free(); _return_to_normal_game())
	dialog.canceled.connect(dialog.queue_free); dialog.popup_centered()

func _return_to_normal_game() -> void:
	_release_machine(false)
	issue_reviewer.set_replay_state("", false)
	issue_reviewer.hide()
	_start_game()

func _cleanup_replay_save() -> void:
	if replay_save_path.is_empty(): return
	DirAccess.remove_absolute(replay_save_path)
	DirAccess.remove_absolute(replay_save_path.get_base_dir())
	replay_save_path = ""; replay_issue = {}; replay_complete = false
	if is_instance_valid(issue_reviewer): issue_reviewer.set_replay_state("", false)

func _review_build() -> String:
	if _review_build_id.is_empty():
		_review_build_id = "dq9_godot sha256:" + FileAccess.get_sha256("res://bin/libdq9_godot.so")
	return _review_build_id

# Short notice at the bottom-left of the game area; "" hides it at once.
func _show_toast(text: String, seconds := 2.5) -> void:
	if _toast == null: return
	_toast_token += 1
	_toast.visible = not text.is_empty()
	_toast.text = text
	if text.is_empty(): return
	var token := _toast_token
	get_tree().create_timer(seconds).timeout.connect(func(): if token == _toast_token: _toast.hide())

# Hold Tab to run unpaced (the pause panel's speed returns on release). Ignored while typing or in dialogs.
func _update_fast_forward() -> void:
	if machine == null:
		if _ff_held: _ff_held = false; _show_toast("")
		return
	var typing := get_viewport().gui_get_focus_owner() is LineEdit or get_viewport().gui_get_focus_owner() is TextEdit
	var want := machine != null and Input.is_key_pressed(KEY_TAB) and not typing and not settings_panel.visible and not issue_reviewer.visible and not pause_panel.visible
	if want == _ff_held: return
	_ff_held = want
	machine.set_speed(0.0 if want else _user_speed)
	if want: _show_toast("FAST-FORWARD >>", 600.0)
	else: _show_toast("")

func _save_screen_hotkey() -> void:
	if machine == null: return
	var dir := "user://screenshots"
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(dir))
	var top: Image = machine.get_top_image()
	var bottom: Image = machine.get_bottom_image()
	if top == null or bottom == null or top.is_empty() or bottom.is_empty():
		_show_toast("Screenshot failed: no frame yet"); return
	var both := Image.create(top.get_width(), top.get_height() + bottom.get_height(), false, top.get_format())
	both.blit_rect(top, Rect2i(Vector2i.ZERO, top.get_size()), Vector2i.ZERO)
	both.blit_rect(bottom, Rect2i(Vector2i.ZERO, bottom.get_size()), Vector2i(0, top.get_height()))
	var stamp := Time.get_datetime_string_from_system().replace(":", "").replace("-", "").replace("T", "-")
	var path := "%s/dq9-%s.png" % [dir, stamp]
	var error := both.save_png(path)
	_show_toast("Screenshot saved: " + path.get_file() if error == OK else "Screenshot failed (%d)" % error)

func _process(_delta: float) -> void:
	_update_fast_forward()
	if machine and machine.has_method("get_stats"):
		var s:Dictionary=machine.get_stats()
		var now := Time.get_ticks_msec()
		if now - _fps_ms >= 1000:
			var fr: int = int(s.get("frame", 0))
			_fps = float(fr - _fps_frame) * 1000.0 / maxf(1.0, float(now - _fps_ms)) if _fps_ms > 0 else 0.0
			_fps_frame = fr; _fps_ms = now
		status_label.text="Frame %d   %.1f FPS   Audio %.1f ms   Underruns %d"%[s.get("frame",0),_fps,s.get("audio_queue_ms",0.0),s.get("underruns",0)]
		if not replay_issue.is_empty() and s.get("replay_active", false):
			issue_reviewer.update_progress(int(s.frame), int(replay_issue.frame), (now - replay_started_ms) / 1000.0)
		status_label.text += "\n" + _gpu3d_status_text()
		var pressed: Array[String] = []
		for action in BUTTON_ACTIONS:
			if Input.is_action_pressed(action): pressed.append(action)
		machine.set_buttons(0 if issue_reviewer.visible else Logic.button_mask(pressed))

func _input(event: InputEvent) -> void:
	if event is InputEventMouseButton and not event.pressed:
		if event.button_index == MOUSE_BUTTON_LEFT and _touching:
			_touching = false
			if machine: machine.set_touch(0,0,false)
		if _overlay_drag or _overlay_resize:
			_overlay_drag = false; _overlay_resize = false; _persist()
	if rebinding_action != "":
		if event is InputEventKey and event.pressed and not event.echo: _replace_binding(event); get_viewport().set_input_as_handled()
		elif event is InputEventJoypadButton and event.pressed: _replace_binding(event); get_viewport().set_input_as_handled()
		elif event is InputEventJoypadMotion and absf(event.axis_value) >= 0.7: _replace_binding(event); get_viewport().set_input_as_handled()
		return
	if event.is_action_pressed("ui_cancel") and settings_panel.visible: _hide_settings(); get_viewport().set_input_as_handled(); return
	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_F7:
			_open_issue_reviewer(); get_viewport().set_input_as_handled(); return
		if issue_reviewer.visible:
			if event.keycode == KEY_ESCAPE:
				_close_issue_reviewer(); get_viewport().set_input_as_handled()
			return # let the focused reviewer LineEdit/TextEdit receive ordinary keys
		if event.keycode==KEY_F12 and machine: _save_screen_hotkey(); get_viewport().set_input_as_handled(); return
		if event.keycode==KEY_F11: get_window().mode=Window.MODE_WINDOWED if get_window().mode==Window.MODE_FULLSCREEN else Window.MODE_FULLSCREEN
		elif event.keycode==KEY_F10: _toggle_pause()
		elif event.keycode==KEY_F6:
			_toggle_gpu3d_compare()
			get_viewport().set_input_as_handled()
		elif event.keycode==KEY_ESCAPE and machine: _toggle_pause()
	var chord := Input.is_action_pressed("ds_start") and Input.is_action_pressed("ds_select")
	if machine and chord and not _chord_latched: _toggle_pause()
	_chord_latched = chord

func _replace_binding(event: InputEvent) -> void:
	InputMap.action_erase_events(rebinding_action); InputMap.action_add_event(rebinding_action,event); rebinding_action=""; _persist(); _refresh_binding_buttons()

func _toggle_pause() -> void:
	if not machine: _show_settings(); return
	_fit_panels()
	pause_panel.visible=not pause_panel.visible; machine.set_paused(pause_panel.visible)
func _persist() -> void:
	settings.overlay_rect = _overlay_rect
	Logic.save_settings("user://settings.cfg", settings)

func _show_settings() -> void: _fit_panels(); settings_panel.show()
func _hide_settings() -> void: settings_panel.hide(); _persist()
func _set_layout(index: int) -> void: layout=index; settings.layout=index; _layout_screens()
func _apply_filter() -> void:
	var mode:=CanvasItem.TEXTURE_FILTER_NEAREST if settings.nearest else CanvasItem.TEXTURE_FILTER_LINEAR
	top_rect.texture_filter=mode; bottom_rect.texture_filter=mode
# Centers the modal panels and keeps them inside the window (they scroll instead of overflowing).
func _fit_panels() -> void:
	var view := get_viewport_rect().size
	for item in [[settings_panel, Vector2(720, 640)], [pause_panel, Vector2(520, 660)]]:
		var panel: PanelContainer = item[0]
		if panel == null or not is_instance_valid(panel): continue
		var wanted: Vector2 = item[1]
		panel.custom_minimum_size = Vector2.ZERO
		panel.size = Vector2(clampf(view.x - 24.0, 280.0, wanted.x), clampf(view.y - 24.0, 240.0, wanted.y))
		panel.position = ((view - panel.size) * 0.5).floor()

func _layout_screens() -> void:
	if not is_instance_valid(game_panel): return
	var rects:=Logic.layout_rects(game_panel.size,layout,_overlay_rect,settings.integer_scale)
	for i in 2:
		var frame:=top_frame if i==0 else bottom_frame
		var r:Rect2=rects[i]
		frame.position=r.position; frame.size=r.size; frame.visible=r.size.x>0 and r.size.y>0
	_apply_filter()
func _on_touch_input(event: InputEvent) -> void:
		var pressed := false
		if layout == 6 and event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT and event.alt_pressed:
			_overlay_drag = event.pressed
			_overlay_start = event.global_position
			_overlay_origin = _overlay_rect.position
			if not event.pressed: _persist()
		elif layout == 6 and event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_RIGHT and event.alt_pressed:
			_overlay_resize = event.pressed
			_overlay_start = event.global_position
			_overlay_size_origin = _overlay_rect.size
			if not event.pressed: _persist()
		elif layout == 6 and event is InputEventMouseMotion and _overlay_drag and (event.button_mask & MOUSE_BUTTON_MASK_LEFT):
			var movement: Vector2 = (event.global_position - _overlay_start) / game_panel.size
			_overlay_rect.position = (_overlay_origin + movement).clamp(Vector2.ZERO, Vector2.ONE - _overlay_rect.size)
			_layout_screens()
		elif layout == 6 and event is InputEventMouseMotion and _overlay_resize and (event.button_mask & MOUSE_BUTTON_MASK_RIGHT):
			var size_delta: Vector2 = (event.global_position - _overlay_start) / game_panel.size
			_overlay_rect.size = (_overlay_size_origin + size_delta).clamp(Vector2(0.15, 0.15), Vector2(0.75, 0.75))
			_overlay_rect.position = _overlay_rect.position.clamp(Vector2.ZERO, Vector2.ONE - _overlay_rect.size)
			_layout_screens()
		if _overlay_drag or _overlay_resize: return
		if event is InputEventScreenTouch: pressed=event.pressed
		elif event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT: pressed=event.pressed
		elif event is InputEventScreenDrag or event is InputEventMouseMotion: pressed=_touching
		_touching=pressed
		# gui_input positions are already local to bottom_rect
		var point:=Logic.touch_coords(event.position,Rect2(Vector2.ZERO,bottom_rect.size))
		if machine: machine.set_touch(point.x,point.y,pressed)
func _save_screenshot() -> void:
	var dir="user://screenshots"; DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(dir))
	var image:=get_viewport().get_texture().get_image(); image.save_png(dir+"/frame_%08d.png"%(_current_frame()))
func _save_bundle() -> void:
	var stamp=Time.get_datetime_string_from_system().replace(":","-").replace(" ","_")
	var dir="user://debug/"+stamp
	var frame := _current_frame()
	if machine and machine.has_method("get_input_history"):
		var bundle_rom: String = replay_issue.env.get("rom_path", settings.rom_path) if not replay_issue.is_empty() else settings.rom_path
		var bundle_save: String = replay_save_path if not replay_save_path.is_empty() else settings.save_path
		var bundle_language: String = replay_issue.language if not replay_issue.is_empty() else settings.language
		var result: String = DebugBundle.save_note(machine, dir, pause_note.text, frame,
			bundle_rom, bundle_save, bundle_language)
		if not result.is_empty():
			push_error(result)
			return
	else:
		DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(dir))
		var top_image: Image = machine.get_top_image() if machine and machine.has_method("get_top_image") else top_rect.texture.get_image()
		var bottom_image: Image = machine.get_bottom_image() if machine and machine.has_method("get_bottom_image") else bottom_rect.texture.get_image()
		top_image.save_png(dir+"/screenshot_top.png")
		bottom_image.save_png(dir+"/screenshot_bottom.png")
		var comment="%s\nframe: %d\n"%[pause_note.text,frame]
		FileAccess.open(dir+"/comment.txt",FileAccess.WRITE).store_string(comment)
		var s:Dictionary=machine.get_stats() if machine else {}
		FileAccess.open(dir+"/state.txt",FileAccess.WRITE).store_string("frame=%d\nfps=%s\naudio_queue_ms=%s\nunderruns=%s\n"%[frame,_fps,s.get("audio_queue_ms",0),s.get("underruns",0)])
	var index="user://debug/index.tsv"
	var f=FileAccess.open(index,FileAccess.READ_WRITE if FileAccess.file_exists(index) else FileAccess.WRITE)
	if f:
		f.seek_end()
		f.store_string("%s\t%s\t%s\t%d\n"%[stamp,stamp,pause_note.text.replace("\t"," ").replace("\n"," "),frame])
		f.close()
func _current_frame() -> int:
	return maxi(0, int(machine.get_stats().get("frame", 0)) - 1) if machine else 0

func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST:
		_release_machine(true)
		get_tree().quit()

func _exit_tree() -> void:
	_release_machine(true)
