extends Control

const Logic = preload("res://scripts/host_logic.gd")
const BUTTON_ACTIONS := Logic.BUTTON_ACTIONS
const BUTTON_BITS := Logic.BUTTON_BITS
const DebugBundle = preload("res://scripts/debug_bundle.gd")
const IssueReviewer = preload("res://scripts/issue_reviewer.gd")
const IssueStore = preload("res://scripts/issue_store.gd")
const Launcher = preload("res://scripts/launcher.gd")
const SaveSlots = preload("res://scripts/save_slots.gd")
const RomArt = preload("res://scripts/rom_art.gd")
const SettingsPages = preload("res://scripts/settings_pages.gd")
const Attract = preload("res://scripts/attract_background.gd")
const LANGS := ["en","fr","de","es","it"]
var settings: Dictionary
var launcher: Control
var attract: Node
var _attract_generation := 0
var _attract_allowed := false
var _attract_config := ""
var _closing := false
var slot_store = SaveSlots.new()
var slot_manager: Control
var slot_inspector: Node
var active_slot := ""
var slot_started_ms := 0
var _migration_checked := false
var machine: Node
var layout := 0
var _user_speed := 1.0          # speed chosen in the pause panel; fast-forward (hold Tab) overrides it
var _ff_held := false
var _toast: Label
var _toast_token := 0
var _chip_hint_shown := false
var _last_save_ok := true
var _machine_last_save_ok := true
var _save_status_text := "Save status: no flush result yet."
var _save_views: Array[Dictionary] = []
var _active_save_config: Dictionary = {}
var _backup_config: Dictionary = {}
var _backup_capacity := 0
var _backup_rows: Array[Dictionary] = []
var _backup_context := "Refresh to list backups for the selected ROM, save and chip."
var _backup_message := ""
var _backup_refresh_pending := false
var _restore_busy := false
var _restore_dialog: ConfirmationDialog
var _restore_confirmation: Dictionary = {}
const MAX_VISIBLE_SAVE_BACKUPS := 50
var settings_panel: PanelContainer
var game_panel: Control
var top_rect: TextureRect
var bottom_rect: TextureRect
var top_frame: PanelContainer
var bottom_frame: PanelContainer
var status_label: Label
var pause_panel: PanelContainer
var pause_note: TextEdit
var _chord_latched := false
var _touching := false
var _overlay_drag := false
var _overlay_resize := false
var _overlay_start := Vector2.ZERO
var _overlay_origin := Vector2.ZERO
var _overlay_size_origin := Vector2.ZERO
var _overlay_rect := Rect2(0.58, 0.56, 0.35, 0.35)
var _single_shapes: Array = []
var _single_nodes: Array[TextureRect] = []
var _single_active := -1
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
var _bundle_active := false
var _review_build_id := ""
# Engine A can appear on either physical screen (POWCNT1 screen swap).
# The producer supplies a mask and a texture from the same completed frame.
var gpu3d_screen_is_top := true
var gpu3d_rect: TextureRect
var _gpu3d_texture: Texture2D
var _gpu3d_mask: Texture2D
var _gpu3d_side_software: Texture2D
var _gpu3d_side_eligibility: Texture2D
# A/B changes presentation only: keep the same GPU scene alive even while
# paused, so comparison never waits for another guest frame or a cache warmup.
var _gpu3d_compare_software := false
const GPU3D_STATUS_FRAMES := 120
var _gpu3d_stats: Dictionary = {}
var _gpu3d_valid_window := PackedByteArray()
var _gpu3d_valid_index := 0
var _gpu3d_valid_count := 0
var _gpu3d_frame_count := 0
var _hd_texture_dump_enabled := false
var _active_widescreen_mode := 0
var _layout_wide_ratio := 1.0
var _layout_wide_screen := 0

func _set_widescreen_mode(mode: int) -> void:
	settings.widescreen_mode = mode
	if machine == null or ((_active_widescreen_mode == 2) == (mode == 2)):
		_active_widescreen_mode = mode
	_apply_gpu3d_settings()
	_persist()

func _update_widescreen_note() -> void:
	if not _ui.has("widescreen_note"): return
	var pending := machine != null and ((_active_widescreen_mode == 2) != (int(settings.widescreen_mode) == 2))
	_ui.widescreen_note.text = "Restart the game to apply Full mode." if pending else ("Widescreen requires GPU hi-res; software fallback remains 4:3." if int(settings.renderer_gpu3d) == 0 and int(settings.widescreen_mode) != 0 else "")


func _ready() -> void:
	get_tree().auto_accept_quit = false
	get_window().title = "DQ9-Remastered"
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
	else: _show_launcher()

func _parse_args() -> void:
	var args := OS.get_cmdline_user_args()
	if settings.rom_path.is_empty():
		settings.rom_path = OS.get_environment("DQ9_ROM")
	var save_env := OS.get_environment("DQ9_SAVE")
	if not save_env.is_empty(): settings.save_path = save_env
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
	top_rect.gui_input.connect(_on_single_screen_input)
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
	settings_overlay.z_index = 11
	add_child(settings_overlay)
	var settings_dim := ColorRect.new()
	settings_dim.color = Color(0.01, 0.02, 0.04, 0.78)
	settings_dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	settings_dim.mouse_filter = Control.MOUSE_FILTER_STOP
	settings_overlay.add_child(settings_dim)
	settings_dim.hide()
	settings_panel = _make_settings()
	settings_overlay.add_child(settings_panel)
	settings_panel.visibility_changed.connect(func(): settings_dim.visible = settings_panel.visible)
	settings_panel.hide()
	var pause_overlay := Control.new()
	pause_overlay.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	pause_overlay.mouse_filter = Control.MOUSE_FILTER_IGNORE
	pause_overlay.z_index = 10
	add_child(pause_overlay)
	var pause_dim := ColorRect.new()
	pause_dim.color = Color(0.02, 0.03, 0.06, 0.55)
	pause_dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	pause_dim.mouse_filter = Control.MOUSE_FILTER_IGNORE
	pause_overlay.add_child(pause_dim)
	pause_panel = _make_pause()
	pause_overlay.add_child(pause_panel)
	pause_panel.visibility_changed.connect(func(): pause_dim.visible = pause_panel.visible)
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
	launcher = Launcher.new()
	launcher.z_index = 5
	add_child(launcher)
	attract = Attract.new()
	add_child(attract)
	attract.frame_ready.connect(launcher.set_live_frame)
	attract.gpu_frame_ready.connect(launcher.set_live_gpu_frame)
	attract.live_started.connect(func(): launcher.set_music_mode(true))
	attract.live_stopped.connect(func(): launcher.clear_live_frame(); launcher.set_music_mode(false))
	launcher.set_menu_music_volume(float(settings.menu_music_volume))
	launcher.set_ui_sfx_volume(float(settings.ui_sfx_volume))
	launcher.set_reduced_motion(bool(settings.menu_reduced_motion))
	launcher.rom_selected.connect(_launcher_rom_selected)
	launcher.wizard_completed.connect(_launcher_wizard_completed)
	launcher.continue_requested.connect(_continue_slot)
	launcher.new_game_requested.connect(_new_game)
	launcher.settings_requested.connect(_show_settings)
	launcher.slots_requested.connect(_show_slot_manager)
	launcher.quit_requested.connect(func(): _quit_app.call_deferred())
	launcher.back_requested.connect(func(): launcher.show_title())
	# GUI hit testing follows tree order, not CanvasItem z_index.
	move_child(pause_overlay, -1)
	move_child(settings_overlay, -1)
	move_child(reviewer_overlay, -1)
	launcher.hide()
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

func _show_launcher() -> void:
	_attract_allowed = not bool(_ui.get("autostart", false))
	if slot_manager:
		slot_manager.queue_free()
		slot_manager = null
	if slot_inspector:
		slot_inspector.free()
		slot_inspector = null
	if not _migration_checked:
		_migration_checked = true
		if slot_store.list_slots().is_empty() and FileAccess.file_exists(settings.save_path):
			slot_store.migrate_legacy(settings.save_path)
	var slots: Array = slot_store.list_slots()
	launcher.set_continue_enabled(not slots.is_empty())
	launcher.set_wizard_choices(settings.language, int(settings.renderer_gpu3d))
	_launcher_rom_selected(settings.rom_path)
	if slots.is_empty() or not _rom_status(settings.rom_path)[1]:
		launcher.show_wizard()
	else:
		launcher.show_title()
	_sync_attract()

func _launcher_rom_selected(path: String) -> void:
	var status := _rom_status(path)
	var previous_path: String = settings.rom_path
	if not status[1]:
		launcher.set_rom("", "", null)
		launcher.set_status(status[0], true)
		_attract_allowed = false
		_attract_config = ""
		_sync_attract()
		return
	_attract_allowed = not bool(_ui.get("autostart", false))
	settings.rom_path = path
	_ensure_abi_config()
	var art: Dictionary = RomArt.load_rom(path, settings.language)
	launcher.set_rom(path, str(art.title), art.texture)
	launcher.set_status(status[0])
	if previous_path != path:
		_attract_config = ""
		_sync_attract()

func _launcher_wizard_completed(language: String, preset: int, slot_id: String, slot_name: String) -> void:
	if not _rom_status(settings.rom_path)[1]:
		launcher.set_status("Choose a valid ROM.", true)
		return
	if slot_id.is_empty():
		var result: Dictionary = slot_store.create_slot(slot_name)
		if not str(result.error).is_empty():
			launcher.set_status(str(result.error), true)
			return
		slot_id = str(result.id)
	settings.language = language
	SettingsPages.apply_preset(settings, preset)
	layout = int(settings.layout)
	_select_slot(slot_id)
	_persist()
	_show_launcher()

func _select_slot(id: String) -> void:
	var slot: Dictionary = slot_store.get_slot(id)
	if slot.is_empty(): return
	active_slot = id
	settings.save_path = str(slot.path)

func _continue_slot() -> void:
	var slots: Array = slot_store.list_slots()
	if slots.is_empty(): return
	var selected := ""
	for slot in slots:
		if str(slot.path) == settings.save_path:
			selected = str(slot.id)
			break
	if selected.is_empty():
		slots.sort_custom(func(a, b): return str(a.updated) > str(b.updated))
		selected = str(slots[0].id)
	_select_slot(selected)
	_persist()
	_start_game()

func _new_game() -> void:
	launcher.set_wizard_choices(settings.language, int(settings.renderer_gpu3d))
	launcher.show_wizard()

func _show_slot_manager() -> void:
	launcher.hide()
	var panel := PanelContainer.new()
	panel.theme = preload("res://scripts/ui_theme.gd").make()
	panel.add_theme_stylebox_override("panel", preload("res://scripts/ui_theme.gd").panel())
	panel.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	panel.z_index = 10
	add_child(panel)
	slot_manager = panel
	var column := VBoxContainer.new(); panel.add_child(column)
	var manager = slot_store.create_manager()
	manager.size_flags_vertical = Control.SIZE_EXPAND_FILL
	column.add_child(manager)
	manager.slot_selected.connect(func(id, _path):
		_select_slot(id)
		if slot_inspector: slot_inspector.free()
		slot_inspector = null
		if ClassDB.class_exists("DqMachine") and not settings.rom_path.is_empty():
			var inspector: Node = ClassDB.instantiate("DqMachine")
			add_child(inspector)
			inspector.set_gpu3d_enabled(false)
			if _load_save_config(inspector, _settings_save_config(false)).is_empty():
				slot_inspector = inspector
				manager.set_backup_machine(inspector)
				_refresh_save_backups()
			else: inspector.free()
	)
	manager.restore_requested.connect(func(id, path):
		_select_slot(id)
		_refresh_save_backups()
		_confirm_restore(path)
	)
	manager.closed.connect(_show_launcher)

func _on_setting_changed(key: String, value: Variant) -> void:
	match key:
		"layout": _set_layout(int(value))
		"nearest": _apply_filter()
		"integer_scale": _layout_screens()
		"window_scale": get_window().size = Vector2i(1280, 800) * int(value)
		"volume": if machine: machine.get_audio_player().volume_linear = float(value)
		"menu_music_volume":
			launcher.set_menu_music_volume(float(value))
			attract.set_volume(float(value))
		"ui_sfx_volume": launcher.set_ui_sfx_volume(float(value))
		"menu_reduced_motion": launcher.set_reduced_motion(bool(value))
		"launcher_live_background": _sync_attract()
		"widescreen_mode":
			_set_widescreen_mode(int(value))
			_ui.settings_pages.refresh(machine != null, _active_widescreen_mode)
		"renderer_gpu3d", "gpu3d_scale", "hd_textures":
			_apply_gpu3d_settings()
			if key == "gpu3d_scale": attract.set_gpu3d_settings(true, int(value))
	_persist()

func _make_settings() -> PanelContainer:
	var panel := PanelContainer.new()
	panel.theme = preload("res://scripts/ui_theme.gd").make()
	panel.add_theme_stylebox_override("panel", preload("res://scripts/ui_theme.gd").panel())
	var pages = SettingsPages.new()
	panel.add_child(pages)
	pages.configure(settings, machine != null, _active_widescreen_mode)
	pages.setting_changed.connect(_on_setting_changed)
	pages.bindings_changed.connect(_persist)
	pages.closed.connect(_hide_settings)
	_ui.settings_pages = pages
	var error := Label.new()
	error.hide()
	add_child(error)
	_ui.error = error
	return panel

func _section(parent: Control, text: String) -> void:
	var label := Label.new(); label.text = text.to_upper()
	label.add_theme_font_size_override("font_size", 12)
	label.add_theme_color_override("font_color", Color(0.45, 0.7, 0.95))
	parent.add_child(label)

# [message, ok] for the launcher ROM selection.
func _rom_status(path: String) -> Array:
	var error: String = RomArt.validate_rom(path)
	if not error.is_empty(): return [error, false]
	var file := FileAccess.open(path, FileAccess.READ)
	var megabytes := int(file.get_length() / 1048576) if file else 0
	return ["ROM found (%d MB)." % megabytes, true]


func _make_pause() -> PanelContainer:
	var p:=PanelContainer.new()
	p.theme = preload("res://scripts/ui_theme.gd").make()
	p.add_theme_stylebox_override("panel", preload("res://scripts/ui_theme.gd").panel(0.88))
	var tabs := TabContainer.new(); p.add_child(tabs)
	_ui.pause_tabs = tabs
	var menu := VBoxContainer.new(); menu.name = "Pause"; menu.add_theme_constant_override("separation", 10); tabs.add_child(menu)
	var title := Label.new(); title.text = "PAUSED"; title.add_theme_font_override("font", load("res://assets/fonts/Cinzel.ttf")); title.add_theme_font_size_override("font_size", 38); menu.add_child(title)
	var resume := Button.new(); resume.text = "Resume"; resume.pressed.connect(_toggle_pause); menu.add_child(resume)
	var settings_button := Button.new(); settings_button.text = "Settings"; settings_button.pressed.connect(_show_settings); menu.add_child(settings_button)
	var tickets := Button.new(); tickets.text = "Tickets"; tickets.pressed.connect(_open_issue_reviewer); menu.add_child(tickets)
	var close := Button.new(); close.text = "Quit to title"; close.pressed.connect(_stop_game); menu.add_child(close)
	var quit := Button.new(); quit.text = "Quit"; quit.pressed.connect(_quit_app); menu.add_child(quit)
	_ui.pause_resume = resume
	var scroll := ScrollContainer.new(); scroll.name = "Developer"; scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED; tabs.add_child(scroll)
	var c:=VBoxContainer.new(); c.add_theme_constant_override("separation",6); c.size_flags_horizontal=Control.SIZE_EXPAND_FILL; scroll.add_child(c)
	var ticket_title := Label.new(); ticket_title.text = "NEW TICKET"; c.add_child(ticket_title)
	pause_note=TextEdit.new(); pause_note.placeholder_text="Describe the problem for a new ticket"; pause_note.custom_minimum_size=Vector2(0,90); c.add_child(pause_note)
	var note:=Button.new(); note.text="Raise ticket (note + screenshots)  [F9]"; note.pressed.connect(_save_bundle); c.add_child(note)
	var reviewer := Button.new(); reviewer.text = "Tickets… (F7)"; reviewer.pressed.connect(_open_issue_reviewer); c.add_child(reviewer)
	var step:=Button.new(); step.text="Step one frame"; step.pressed.connect(func():machine.set_paused(true);machine.step_once()); c.add_child(step)
	var speeds:=OptionButton.new()
	for label in ["0.25×","0.5×","1×","2×","Max"]: speeds.add_item(label)
	speeds.item_selected.connect(func(i): _user_speed = [0.25,0.5,1.0,2.0,0.0][i]; machine.set_speed(_user_speed))
	speeds.select(2); c.add_child(speeds)
	var stats:=CheckButton.new(); stats.text="Show statistics"; stats.button_pressed=true; stats.toggled.connect(func(v):status_label.visible=v); c.add_child(stats)
	var shot:=Button.new(); shot.text="Save screenshot"; shot.pressed.connect(_save_screenshot); c.add_child(shot)
	var texture_dump := CheckButton.new(); texture_dump.name = "TextureDump"
	texture_dump.text = "Dump 3D textures"; texture_dump.button_pressed = _hd_texture_dump_enabled
	texture_dump.toggled.connect(func(v): _hd_texture_dump_enabled = v; if machine and machine.has_method("set_hd_texture_dump_enabled"): machine.set_hd_texture_dump_enabled(v))
	c.add_child(texture_dump)
	_make_save_controls(c, "Pause")
	return p


func _make_save_controls(parent: Control, prefix: String) -> void:
	_section(parent, "Save backups")
	var status := Label.new(); status.name = prefix + "SaveStatus"
	status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART; parent.add_child(status)
	var context := Label.new(); context.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	context.add_theme_font_size_override("font_size", 13); parent.add_child(context)
	var list := ItemList.new(); list.name = prefix + "SaveBackups"
	list.custom_minimum_size.y = 100; list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	list.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS; parent.add_child(list)
	var row := HBoxContainer.new(); parent.add_child(row)
	var refresh := Button.new(); refresh.text = "Refresh backups"; refresh.pressed.connect(_refresh_save_backups); row.add_child(refresh)
	var restore := Button.new(); restore.text = "Restore…"; restore.disabled = true
	restore.pressed.connect(func():
		var selected := list.get_selected_items()
		if not selected.is_empty(): _confirm_restore(str(list.get_item_metadata(selected[0]).path))
	); row.add_child(restore)
	var message := Label.new(); message.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	message.add_theme_color_override("font_color", Color(1.0, 0.7, 0.4)); parent.add_child(message)
	_save_views.append({"status": status, "context": context, "list": list, "restore": restore, "refresh": refresh, "message": message})
	list.item_selected.connect(func(_index): _update_restore_buttons())
	_update_save_views()

func _ensure_abi_config() -> String:
	if OS.get_environment("DQ9_ABI_CONFIG").is_empty():
		var abi := Logic.find_abi_config(ProjectSettings.globalize_path("res://"))
		if abi.is_empty(): abi = Logic.find_abi_config(OS.get_executable_path().get_base_dir())
		if not abi.is_empty():
			OS.set_environment("DQ9_ABI_CONFIG", abi)
			print("Using ABI configuration ", abi)
	return OS.get_environment("DQ9_ABI_CONFIG")

func _settings_save_config(_typed := true) -> Dictionary:
	var rom: String = settings.rom_path
	var save: String = settings.save_path
	var language: String = settings.language
	var chip: String = settings.backup_type
	return {"rom_path": ProjectSettings.globalize_path(rom) if not rom.is_empty() else "",
		"save_path": ProjectSettings.globalize_path(save) if not save.is_empty() else "",
		"language": language, "backup_type": chip, "abi_config": _ensure_abi_config()}

func _current_save_config() -> Dictionary:
	# Settings can be edited while playing. Never redirect a live/replay restore
	# to those paths or reinterpret its cartridge with the newly selected chip.
	return _active_save_config.duplicate(true) if machine else _settings_save_config()

func _load_save_config(target: Node, config: Dictionary) -> String:
	var previous_abi := OS.get_environment("DQ9_ABI_CONFIG")
	if str(config.abi_config).is_empty(): OS.unset_environment("DQ9_ABI_CONFIG")
	else: OS.set_environment("DQ9_ABI_CONFIG", config.abi_config)
	target.set_backup_type(config.backup_type)
	var result: String = target.load_game(config.rom_path, config.save_path, Logic.firmware_language(config.language))
	if previous_abi.is_empty(): OS.unset_environment("DQ9_ABI_CONFIG")
	else: OS.set_environment("DQ9_ABI_CONFIG", previous_abi)
	return result

func _is_issue_replay() -> bool:
	return not replay_issue.is_empty() or not replay_save_path.is_empty() or (machine != null and bool(machine.get_stats().get("replay_active", false)))


func _queue_save_backup_refresh() -> void:
	if _backup_refresh_pending or _restore_busy: return
	_backup_refresh_pending = true
	call_deferred("_refresh_queued_save_backups")

func _refresh_queued_save_backups() -> void:
	if not _backup_refresh_pending: return
	_backup_refresh_pending = false
	if settings_panel.visible or pause_panel.visible: _refresh_save_backups()

func _refresh_save_backups() -> void:
	if _restore_busy: return
	_backup_refresh_pending = false
	_backup_rows.clear(); _backup_capacity = 0; _backup_message = ""
	_backup_config = _current_save_config()
	_backup_context = "No loaded save configuration."
	var source: Node = machine
	var temporary: Node
	if _backup_config.is_empty():
		_backup_message = "The current game's save configuration is unavailable. Close it before restoring."
	elif not ClassDB.class_exists("DqMachine"):
		_backup_message = "Build the native extension to list and restore save backups."
	elif str(_backup_config.rom_path).is_empty() or str(_backup_config.save_path).is_empty():
		_backup_message = "Choose a ROM and save path, then refresh backups."
	else:
		if source == null:
			# Inspect only on panel-open/explicit refresh, never per frame or key.
			# This machine is loaded but never started or stepped; its clean
			# destruction does not create a save or rotate existing backups.
			temporary = ClassDB.instantiate("DqMachine"); add_child(temporary)
			temporary.set_gpu3d_enabled(false)
			var result := _load_save_config(temporary, _backup_config)
			if result.is_empty(): source = temporary
			else: _backup_message = result
		if source != null:
			if not source.has_method("get_save_backups") or not source.has_method("get_save_backup_error") or not source.has_method("get_save_capacity") or not source.has_method("restore_save"):
				_backup_message = "Rebuild the native extension for save backup support."
			else:
				_backup_capacity = int(source.get_save_capacity())
				var backups: Array = source.get_save_backups()
				var listing_error: String = source.get_save_backup_error()
				if listing_error.is_empty():
					_backup_rows = Logic.save_backup_rows(backups, _backup_capacity, MAX_VISIBLE_SAVE_BACKUPS)
				var origin := "Current game" if machine else "Settings (game stopped)"
				if _is_issue_replay(): origin = "Issue replay copy — restore disabled"
				_backup_context = "%s: %s\nChip request: %s · capacity: %d B · newest first (UTC)" % [origin, _backup_config.save_path, _backup_config.backup_type, _backup_capacity]
				if backups.size() > MAX_VISIBLE_SAVE_BACKUPS:
					_backup_context += "\nShowing the newest %d of %d backups." % [MAX_VISIBLE_SAVE_BACKUPS, backups.size()]
				if not listing_error.is_empty(): _backup_message = "Cannot list save backups: " + listing_error
				elif backups.is_empty(): _backup_message = "No timestamped backups for this save."
				if _is_issue_replay():
					if not _backup_message.is_empty(): _backup_message += "\n"
					_backup_message += "Restore is disabled throughout issue replay, including Play from here. Return to the normal game first."
	if temporary != null: temporary.free()
	_update_save_views()

func _update_save_views() -> void:
	for view in _save_views:
		var list: ItemList = view.list
		var selected_path := ""
		var selected := list.get_selected_items()
		if not selected.is_empty(): selected_path = str(list.get_item_metadata(selected[0]).path)
		list.clear()
		var selected_index := 0
		for index in _backup_rows.size():
			var backup: Dictionary = _backup_rows[index]
			list.add_item(backup.label + (" — incompatible" if not str(backup.error).is_empty() else ""))
			list.set_item_metadata(index, backup)
			list.set_item_tooltip(index, str(backup.path) + ("\n" + str(backup.error) if not str(backup.error).is_empty() else ""))
			if backup.path == selected_path: selected_index = index
		if not _backup_rows.is_empty(): list.select(selected_index)
		view.context.text = _backup_context
		view.status.text = _save_status_text
		view.status.add_theme_color_override("font_color", Color(0.5, 0.85, 0.6) if _last_save_ok else Color(1.0, 0.45, 0.4))
	_update_restore_buttons()

func _update_restore_buttons() -> void:
	var blocked := _restore_busy or _is_issue_replay()
	for view in _save_views:
		var list: ItemList = view.list
		var selected := list.get_selected_items()
		var error := ""
		if not selected.is_empty(): error = str(list.get_item_metadata(selected[0]).error)
		view.restore.disabled = blocked or selected.is_empty() or not error.is_empty()
		view.restore.tooltip_text = error
		view.refresh.disabled = _restore_busy
		view.message.text = _backup_message if not _backup_message.is_empty() else error

func _restore_candidate_error(path: String) -> String:
	if _is_issue_replay(): return "Restore is disabled during issue replay. Return to the normal game first."
	if _backup_rows.is_empty() and not _backup_message.is_empty(): return _backup_message
	for backup in _backup_rows:
		if backup.path == path: return Logic.save_restore_error(int(backup.size), _backup_capacity)
	return "This backup is no longer listed for the current save. Refresh and select it again."

func _show_restore_message(message: String) -> void:
	_backup_message = message
	_update_restore_buttons()
	_show_toast(message, 12.0)

func _confirm_restore(path: String) -> void:
	if _restore_busy or is_instance_valid(_restore_dialog): return
	_refresh_save_backups()
	var error := _restore_candidate_error(path)
	if not error.is_empty(): _show_restore_message(error); return
	_restore_confirmation = {"path": path, "config": _backup_config.duplicate(true), "capacity": _backup_capacity}
	var dialog := ConfirmationDialog.new(); _restore_dialog = dialog
	dialog.title = "Restore cartridge save backup?"
	dialog.dialog_autowrap = true
	dialog.dialog_text = "Restore %s to:\n%s\n\nThe current on-disk save is backed up before replacement. Progress after the selected backup will be replaced; this is not a save state. The save chip will not change (%d B).\n\n%s" % [
		Logic.save_backup_label(path, int(_backup_capacity)), _backup_config.save_path, _backup_capacity,
		"The current game will be flushed, closed and restarted. If the flush fails, restore is cancelled and the live game is kept." if machine else "The game will remain closed after restoring."]
	dialog.ok_button_text = "Restore and restart" if machine else "Restore (stay closed)"
	add_child(dialog)
	dialog.confirmed.connect(func(): _restore_dialog = null; dialog.queue_free(); _restore_backup(path))
	dialog.canceled.connect(func(): _restore_dialog = null; _restore_confirmation = {}; dialog.queue_free())
	dialog.popup_centered_clamped(Vector2i(620, 340), 0.9)

func _cleanup_restore_source(directory: String) -> void:
	if directory.is_empty(): return
	DirAccess.remove_absolute(directory.path_join("restore.sav"))
	DirAccess.remove_absolute(directory)

func _restore_backup(path: String) -> void:
	if _restore_busy: return
	var confirmation := _restore_confirmation
	_restore_confirmation = {}
	_refresh_save_backups()
	var error := _restore_candidate_error(path)
	if error.is_empty() and not confirmation.is_empty() and (confirmation.path != path or confirmation.config != _backup_config or int(confirmation.capacity) != _backup_capacity):
		error = "The save configuration changed after confirmation. Refresh and confirm the restore again."
	if not error.is_empty(): _show_restore_message(error); return
	var config := _backup_config.duplicate(true)
	var capacity := _backup_capacity
	var restart := machine != null
	var source := path
	var staging := ""
	_restore_busy = true; _update_restore_buttons()
	if restart:
		# A first-write flush can rotate away the selected oldest backup. Keep a
		# private copy before touching the live session; native still validates it.
		staging = OS.get_cache_dir().path_join("dq9-remastered/restore-%d-%d" % [OS.get_process_id(), Time.get_ticks_usec()])
		source = staging.path_join("restore.sav")
		if DirAccess.make_dir_recursive_absolute(staging) != OK or DirAccess.copy_absolute(path, source) != OK:
			_cleanup_restore_source(staging)
			_restore_busy = false; _show_restore_message("Cannot preserve the selected backup for restore. The current game has not been closed.")
			return
		if not _release_machine(false, true):
			_cleanup_restore_source(staging)
			_restore_busy = false; _show_restore_message("Restore cancelled: save flush failed. The live game is still open; repair the save path and retry.")
			return
	var restored: Node = ClassDB.instantiate("DqMachine"); add_child(restored)
	if restart:
		_active_widescreen_mode = int(settings.widescreen_mode)
		restored.set_widescreen_camera(_active_widescreen_mode == 2)
	restored.set_gpu3d_enabled(false)
	var timezone := Time.get_time_zone_from_system()
	restored.set_rtc_epoch(int(Time.get_unix_time_from_system()), int(timezone.get("bias", 0)))
	error = _load_save_config(restored, config)
	if error.is_empty():
		var actual_capacity: int = restored.get_save_capacity()
		error = Logic.save_restore_error(capacity, actual_capacity)
		if error.is_empty(): error = restored.restore_save(source)
	_cleanup_restore_source(staging)
	if error.is_empty():
		_observe_save_result(true, "Saved — backup restored to disk.")
		if restart:
			machine = restored; _active_save_config = config
			_chip_hint_shown = false; _machine_last_save_ok = true
			_user_speed = 1.0; _ff_held = false
			_apply_gpu3d_settings(); _start_loaded_game()
		else:
			restored.free()
	else:
		restored.free()
		pause_panel.hide(); _show_settings()
	_restore_busy = false
	_refresh_save_backups()
	if error.is_empty(): _show_restore_message("Backup restored. Game restarted." if restart else "Backup restored. Game remains closed.")
	else: _show_restore_message("Restore failed; game remains closed: " + error)

func _sync_attract() -> void:
	_attract_generation += 1
	var generation := _attract_generation
	var wanted: bool = _attract_allowed and bool(settings.get("launcher_live_background", true)) and bool(_rom_status(settings.rom_path)[1])
	var config := "%s\n%s\n%s" % [settings.rom_path, settings.language, _ensure_abi_config()] if wanted else ""
	if wanted and attract.active and _attract_config == config:
		return
	_attract_config = ""
	await attract.release()
	if generation != _attract_generation: return
	if not wanted: return
	attract.set_volume(float(settings.get("menu_music_volume", 0.7)))
	attract.set_gpu3d_settings(true, int(settings.gpu3d_scale))
	var error: String = attract.start(settings.rom_path, _ensure_abi_config(), settings.language)
	if error.is_empty(): _attract_config = config
	else: push_warning("Could not start launcher background: " + error)

func _release_attract_for_game() -> void:
	_attract_allowed = false
	_attract_generation += 1
	await attract.release()
	_attract_config = ""

func _start_game() -> void:
	await _release_attract_for_game()
	_ui.error.text = ""
	if settings.rom_path.is_empty():
		_ui.error.text = "Choose a Nintendo DS ROM before starting."
		_show_launcher(); launcher.show_wizard(); launcher.set_status(str(_ui.error.text), true)
		return
	_release_machine(false) # flush the old save and stop its worker before the replacement loads from disk
	issue_reviewer.set_replay_state("", false)
	if not ClassDB.class_exists("DqMachine"):
		_ui.error.text = "Native extension unavailable. Build the Godot extension before starting a game."
		_show_launcher(); launcher.show_wizard(); launcher.set_status(str(_ui.error.text), true)
		return
	machine=ClassDB.instantiate("DqMachine")
	add_child(machine)
	_active_widescreen_mode = int(settings.widescreen_mode)
	machine.set_widescreen_camera(_active_widescreen_mode == 2)
	_chip_hint_shown = false
	_machine_last_save_ok = true
	_user_speed = 1.0; _ff_held = false
	_apply_gpu3d_settings()
	if machine.has_method("set_rtc_epoch"):
		var timezone := Time.get_time_zone_from_system()
		machine.set_rtc_epoch(int(Time.get_unix_time_from_system()), int(timezone.get("bias", 0)))
	var config := _settings_save_config(false)
	var result := _load_save_config(machine, config)
	if not result.is_empty():
		push_error(result)
		_ui.error.text = result
		_release_machine(false)
		_show_launcher(); launcher.show_wizard(); launcher.set_status(result, true)
		return
	_active_save_config = config
	_start_loaded_game()

func _start_loaded_game() -> void:
	launcher.hide()
	if not active_slot.is_empty() and not _is_issue_replay(): slot_started_ms = Time.get_ticks_msec()
	machine.start()
	_on_machine_frame(0)
	if machine.has_signal("frame_ready"): machine.frame_ready.connect(_on_machine_frame)
	machine.get_audio_player().volume_linear=settings.volume
	pause_panel.hide()
	_hide_settings()
	_queue_save_backup_refresh()

func _on_machine_frame(_frame: int) -> void:
	if machine == null or not is_instance_valid(machine): return
	top_rect.texture=machine.get_top_texture()
	bottom_rect.texture=machine.get_bottom_texture()
	if layout == 7:
		var snapshot: Dictionary = machine.get_ram_snapshot() if machine.has_method("get_ram_snapshot") else {}
		var shapes: Array = Logic.detect_single_screen_shapes(snapshot, settings.single_screen_shapes)
		if shapes != _single_shapes:
			_single_shapes = shapes.duplicate(true)
			_layout_screens()
		# Textures are updated after the GPU composite selects its presentation.
	if int(settings.renderer_gpu3d) != 0 and machine.has_method("get_gpu3d_texture") and machine.has_method("get_gpu3d_mask_texture"):
		var screen_is_top := gpu3d_screen_is_top
		if machine.has_method("get_gpu3d_screen_is_top"):
			screen_is_top = bool(machine.get_gpu3d_screen_is_top())
		var side_software: Texture2D = machine.get_gpu3d_side_software_texture() if machine.has_method("get_gpu3d_side_software_texture") else null
		var side_eligibility: Texture2D = machine.get_gpu3d_side_eligibility_texture() if machine.has_method("get_gpu3d_side_eligibility_texture") else null
		set_gpu3d_frame(machine.get_gpu3d_texture(), machine.get_gpu3d_mask_texture(), screen_is_top, side_software, side_eligibility)
		if machine.has_method("get_gpu3d_stats"):
			_gpu3d_stats = machine.get_gpu3d_stats()
			_observe_gpu3d_frame()
	if layout == 7:
		for node in _single_nodes:
			node.texture = bottom_rect.texture

# Integration point also used by synthetic producers. Null textures select the
# software result for this frame (unsupported GPU effects, shadows, etc.).
func set_gpu3d_frame(texture: Texture2D, mask: Texture2D, screen_is_top := true, side_software: Texture2D = null, side_eligibility: Texture2D = null) -> void:
	_gpu3d_texture = texture
	_gpu3d_mask = mask
	_gpu3d_side_software = side_software
	_gpu3d_side_eligibility = side_eligibility
	gpu3d_screen_is_top = screen_is_top
	_update_gpu3d_composite()

func _apply_gpu3d_settings() -> void:
	var enabled := int(settings.renderer_gpu3d) != 0
	if _ui.has("gpu_scale"): _ui.gpu_scale.disabled = not enabled
	_update_widescreen_note()
	if _ui.has("gpu_compare"): _ui.gpu_compare.disabled = not enabled
	if not enabled:
		_gpu3d_compare_software = false
		_clear_gpu3d_status()
	if machine:
		if machine.has_method("set_gpu3d_scale"): machine.set_gpu3d_scale(int(settings.gpu3d_scale))
		if machine.has_method("set_gpu3d_widescreen_enabled"): machine.set_gpu3d_widescreen_enabled(_active_widescreen_mode != 0)
		if machine.has_method("set_gpu3d_guest_widescreen"): machine.set_gpu3d_guest_widescreen(_active_widescreen_mode == 2)
		if machine.has_method("set_hd_textures_enabled"): machine.set_hd_textures_enabled(bool(settings.hd_textures))
		if machine.has_method("set_hd_texture_dump_enabled"): machine.set_hd_texture_dump_enabled(_hd_texture_dump_enabled)
		if machine.has_method("set_gpu3d_enabled"): machine.set_gpu3d_enabled(enabled)
		# Disabling releases the producer's viewport. Re-enabling must not
		# resurrect its cached texture/mask while the new epoch warms up.
		if machine.has_method("get_gpu3d_texture") and machine.has_method("get_gpu3d_mask_texture"):
			_gpu3d_texture = machine.get_gpu3d_texture()
			_gpu3d_mask = machine.get_gpu3d_mask_texture()
			_gpu3d_side_software = machine.get_gpu3d_side_software_texture() if machine.has_method("get_gpu3d_side_software_texture") else null
			_gpu3d_side_eligibility = machine.get_gpu3d_side_eligibility_texture() if machine.has_method("get_gpu3d_side_eligibility_texture") else null
	_update_gpu3d_composite()

func _gpu_width_ratio() -> float:
	if _active_widescreen_mode != 0 and _gpu3d_texture != null and _gpu3d_texture.get_height() > 0:
		return float(_gpu3d_texture.get_width()) / float(_gpu3d_texture.get_height()) * 0.75
	return 1.0

func _update_gpu3d_composite() -> void:
	var ratio := _gpu_width_ratio() if int(settings.renderer_gpu3d) != 0 and not _gpu3d_compare_software and _gpu3d_texture != null and _gpu3d_mask != null else 1.0
	var wide_screen := 0 if gpu3d_screen_is_top or ratio <= 1.0 else 1
	if not is_equal_approx(ratio, _layout_wide_ratio) or wide_screen != _layout_wide_screen:
		_layout_wide_ratio = ratio
		_layout_wide_screen = wide_screen
		_layout_screens()
	if machine and is_instance_valid(top_rect) and is_instance_valid(bottom_rect):
		var use_presentation := _active_widescreen_mode == 2 and int(settings.renderer_gpu3d) != 0 and not _gpu3d_compare_software and _gpu3d_texture != null and _gpu3d_mask != null and machine.has_method("get_presentation_top_texture")
		top_rect.texture = machine.get_presentation_top_texture() if use_presentation else machine.get_top_texture()
		bottom_rect.texture = machine.get_presentation_bottom_texture() if use_presentation else machine.get_bottom_texture()
		for node in _single_nodes:
			node.texture = bottom_rect.texture
	_update_single_gpu()
	if is_instance_valid(top_frame): top_frame.clip_contents = true
	if is_instance_valid(bottom_frame): bottom_frame.clip_contents = true
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
	# The GPU child extends beyond the physical screen; 2D and touch stay native.
	# The frame must not clip those side regions, while game_panel still clips.
	if _active_widescreen_mode != 0 and _gpu3d_texture.get_height() > 0:
		(screen.get_parent() as Control).clip_contents = false
	gpu3d_rect.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	gpu3d_rect.anchor_left = (1.0 - ratio) * 0.5
	gpu3d_rect.anchor_right = (1.0 + ratio) * 0.5
	gpu3d_rect.texture = _gpu3d_texture
	var material := gpu3d_rect.material as ShaderMaterial
	material.set_shader_parameter("three_d_mask", _gpu3d_mask)
	material.set_shader_parameter("width_ratio", ratio)
	var side_enabled := _active_widescreen_mode == 2 and _gpu3d_side_software != null and _gpu3d_side_eligibility != null
	material.set_shader_parameter("side_fallback_enabled", side_enabled)
	if side_enabled:
		material.set_shader_parameter("side_software_texture", _gpu3d_side_software)
		material.set_shader_parameter("side_eligibility_texture", _gpu3d_side_eligibility)
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

func _release_machine(drain_audio: bool, keep_on_save_failure := false) -> bool:
	var closing = machine
	if closing == null or not is_instance_valid(closing): return true
	var was_paused: bool = closing.is_paused()
	# Pause without stopping first, so an unsuccessful restore preflight can
	# retain and resume this exact session, including its pending in-memory save.
	if keep_on_save_failure: closing.set_paused(true)
	elif closing.has_method("stop"): closing.stop()
	if closing.has_method("flush_save") and closing.get_stats().get("loaded", false):
		var save_ok: bool = closing.flush_save()
		_observe_save_result(save_ok)
		if not save_ok:
			if keep_on_save_failure:
				closing.set_paused(was_paused)
				_observe_save_result(false, "Save failed — restore cancelled; live game kept open.")
				return false
			var warning := "SAVE NOT WRITTEN BEFORE CLOSE: CHECK SAVE PATH"
			_save_status_text = "Save failed — close flush failed; final shutdown retry is not confirmed."
			_observe_save_result(false, _save_status_text)
			_show_toast(warning, 15.0)
			_ui.error.text = warning
			push_warning("Save flush failed before closing. The final shutdown retry is best effort; the latest save data may not be on disk.")
	if slot_started_ms > 0 and not active_slot.is_empty():
		slot_store.update_playtime(active_slot, int((Time.get_ticks_msec() - slot_started_ms) / 1000))
		slot_started_ms = 0
	if closing.has_method("stop"): closing.stop()
	machine = null; _active_save_config = {}
	_update_widescreen_note()
	set_gpu3d_frame(null, null)
	_gpu3d_compare_software = false; _clear_gpu3d_status()
	_ff_held = false
	if closing.has_signal("frame_ready"):
		var callback := Callable(self, "_on_machine_frame")
		if closing.is_connected("frame_ready", callback): closing.disconnect("frame_ready", callback)
	var audio_player: AudioStreamPlayer = closing.get_audio_player() if closing.has_method("get_audio_player") else null
	if audio_player: audio_player.stop()
	closing.free()
	_cleanup_replay_save()
	if drain_audio: OS.delay_msec(500)
	return true

func _stop_game() -> void:
	_release_machine(true)
	pause_panel.hide(); _show_launcher()

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
		if _reviewer_was_paused:
			pause_panel.show()
			_ui.pause_resume.grab_focus()
	else: _show_launcher()

func _start_issue_replay(issue: Dictionary, record_hashes := false) -> void:
	await _release_attract_for_game()
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
	var temporary_dir := OS.get_cache_dir().path_join("dq9-remastered/issue-replay-%d-%d" % [OS.get_process_id(), Time.get_ticks_usec()])
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
	_active_widescreen_mode = clampi(int(environment.get("widescreen_mode", 0)), 0, 2)
	machine.set_widescreen_camera(_active_widescreen_mode == 2)
	_machine_last_save_ok = true
	# Software screenshots are the exact reference even when normal play uses GPU 3D.
	machine.set_gpu3d_enabled(false)
	var backup_type: String = str(environment.get("backup_type", "auto"))
	if backup_type not in Logic.BACKUP_TYPES: backup_type = "auto"
	var metadata := IssueReviewer.Store.fields(issue.comment)
	machine.set_rtc_epoch(int(environment.get("rtc_epoch", metadata.get("rtc_epoch", 0))), int(environment.get("rtc_timezone_offset_minutes", metadata.get("rtc_timezone_offset_minutes", 0))))
	_active_save_config = {"rom_path": ProjectSettings.globalize_path(rom_path),
		"save_path": replay_save_path, "language": str(issue.language),
		"backup_type": backup_type, "abi_config": _ensure_abi_config()}
	var error := _load_save_config(machine, _active_save_config)
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
	_queue_save_backup_refresh()

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

func _observe_save_result(save_ok: bool, detail := "") -> void:
	var changed := save_ok != _last_save_ok
	_last_save_ok = save_ok; _machine_last_save_ok = save_ok
	_save_status_text = detail if not detail.is_empty() else ("Saved — last flush succeeded (or no pending writes)." if save_ok else "Save failed — repair the save path and retry before closing.")
	for view in _save_views:
		view.status.text = _save_status_text
		view.status.add_theme_color_override("font_color", Color(0.5, 0.85, 0.6) if save_ok else Color(1.0, 0.45, 0.4))
	if not changed: return
	if save_ok:
		_show_toast("SAVE WRITTEN: SAVE PATH WORKING AGAIN", 6.0)
		if str(_ui.error.text).begins_with("SAVE NOT WRITTEN"): _ui.error.text = ""
	else:
		_show_toast("SAVE NOT WRITTEN: CHECK SAVE PATH (WILL RETRY ON NEXT SAVE)", 15.0)

# Hold Tab to run unpaced (the pause panel's speed returns on release). Ignored while typing or in dialogs.
func _update_fast_forward() -> void:
	if machine == null:
		if _ff_held: _ff_held = false; _show_toast("")
		return
	var typing := get_viewport().gui_get_focus_owner() is LineEdit or get_viewport().gui_get_focus_owner() is TextEdit
	var want := machine != null and Input.is_key_pressed(KEY_TAB) and not typing and not settings_panel.visible and not issue_reviewer.visible and not pause_panel.visible and not is_instance_valid(_restore_dialog) and not _restore_busy
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
		if s.get("chip_mismatch", false) and not _chip_hint_shown:
			_chip_hint_shown = true
			_show_toast("Save chip wrong? Try EEPROM 64 KB (Start / Settings)", 12.0)
		var save_ok := bool(s.get("last_save_ok", true))
		if save_ok != _machine_last_save_ok: _observe_save_result(save_ok)
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
		machine.set_buttons(0 if issue_reviewer.visible or settings_panel.visible or pause_panel.visible or is_instance_valid(_restore_dialog) or _restore_busy else Logic.button_mask(pressed))

func _input(event: InputEvent) -> void:
	if (event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT and not event.pressed) or (event is InputEventScreenTouch and not event.pressed):
		if _touching:
			_touching = false
			_single_active = -1
			if machine: machine.set_touch(0,0,false)
	if event is InputEventMouseButton and not event.pressed:
		if _overlay_drag or _overlay_resize:
			_overlay_drag = false; _overlay_resize = false; _persist()
	if _bundle_active:
		get_viewport().set_input_as_handled()
		return
	if _restore_busy or is_instance_valid(_restore_dialog): return
	if settings_panel.visible and _ui.settings_pages.handle_rebind(event):
		get_viewport().set_input_as_handled()
		return
	if event.is_action_pressed("ui_cancel") and settings_panel.visible: _hide_settings(); get_viewport().set_input_as_handled(); return
	if event.is_action_pressed("ui_cancel") and pause_panel.visible and not issue_reviewer.visible:
		_toggle_pause(); get_viewport().set_input_as_handled(); return
	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_F7:
			_open_issue_reviewer(); get_viewport().set_input_as_handled(); return
		if issue_reviewer.visible:
			if event.keycode == KEY_ESCAPE:
				_close_issue_reviewer(); get_viewport().set_input_as_handled()
			return # let the focused reviewer LineEdit/TextEdit receive ordinary keys
		if event.keycode==KEY_F12 and machine: _save_screen_hotkey(); get_viewport().set_input_as_handled(); return
		if event.keycode==KEY_F11: get_window().mode=Window.MODE_WINDOWED if get_window().mode==Window.MODE_FULLSCREEN else Window.MODE_FULLSCREEN
		elif event.keycode==KEY_F9 and machine:
			# F9 raises a ticket from the pause note; with no note yet, open the
			# pause panel so the problem can be described first.
			if pause_note.text.strip_edges().is_empty() and (not pause_panel.visible or _ui.pause_tabs.current_tab != 1):
				if not pause_panel.visible: _toggle_pause()
				_ui.pause_tabs.current_tab = 1; pause_note.grab_focus()
				_show_toast("Describe the problem, then press F9 or Raise ticket", 8.0)
			else:
				_save_bundle()
			get_viewport().set_input_as_handled()
		elif event.keycode==KEY_F10: _toggle_pause()
		elif event.keycode==KEY_F6:
			_toggle_gpu3d_compare()
			get_viewport().set_input_as_handled()
		elif event.keycode==KEY_ESCAPE and machine: _toggle_pause()
	var chord := Input.is_action_pressed("ds_start") and Input.is_action_pressed("ds_select")
	if machine and chord and not _chord_latched: _toggle_pause()
	_chord_latched = chord


func _toggle_pause() -> void:
	if not machine: _show_settings(); return
	_fit_panels()
	pause_panel.visible=not pause_panel.visible; machine.set_paused(pause_panel.visible)
	if pause_panel.visible:
		_ui.pause_resume.grab_focus()
		_queue_save_backup_refresh()
func _persist() -> void:
	settings.overlay_rect = _overlay_rect
	Logic.save_settings("user://settings.cfg", settings)

func _show_settings() -> void:
	if machine and not pause_panel.visible:
		_fit_panels(); pause_panel.show(); machine.set_paused(true)
	_fit_panels()
	_ui.settings_pages.refresh(machine != null, _active_widescreen_mode)
	settings_panel.show()
	_fit_panels.call_deferred()
func _hide_settings() -> void:
	settings_panel.hide()
	_persist()
	if pause_panel.visible: _ui.pause_resume.grab_focus()
	elif launcher.visible: launcher.show_title()
func _set_layout(index: int) -> void:
	if index != layout and _touching:
		if machine: machine.set_touch(0, 0, false)
		_touching = false
		_single_active = -1
	layout=index; settings.layout=index; _layout_screens()
func _apply_filter() -> void:
	var mode:=CanvasItem.TEXTURE_FILTER_NEAREST if settings.nearest else CanvasItem.TEXTURE_FILTER_LINEAR
	top_rect.texture_filter=mode; bottom_rect.texture_filter=mode
	for node in _single_nodes:
		node.texture_filter = mode
		(node.material as ShaderMaterial).set_shader_parameter("linear_filter", not settings.nearest)
# Centers the modal panels and keeps them inside the window (they scroll instead of overflowing).
func _fit_panels() -> void:
	var view := get_viewport_rect().size
	for item in [[settings_panel, Vector2(minf(view.x * 0.75, 1440.0), 590)], [pause_panel, Vector2(520, 660)]]:
		var panel: PanelContainer = item[0]
		if panel == null or not is_instance_valid(panel): continue
		var wanted: Vector2 = item[1]
		panel.custom_minimum_size = Vector2.ZERO
		panel.size = Vector2(minf(maxf(view.x - 24.0, 0.0), wanted.x), minf(maxf(view.y - 24.0, 0.0), wanted.y))
		panel.position = ((view - panel.size) * 0.5).floor()
		if panel == pause_panel: panel.position.x = 24.0

func _layout_screens() -> void:
	if not is_instance_valid(game_panel): return
	var rects:=Logic.layout_rects(game_panel.size,layout,_overlay_rect,settings.integer_scale,_layout_wide_ratio,_layout_wide_screen)
	for i in 2:
		var frame:=top_frame if i==0 else bottom_frame
		var r:Rect2=rects[i]
		frame.position=r.position; frame.size=r.size; frame.visible=r.size.x>0 and r.size.y>0
	_apply_filter()
	_update_single_screen()

func _update_single_screen() -> void:
	for node in _single_nodes: node.free()
	_single_nodes.clear()
	if layout != 7: return
	var snapshot: Dictionary = machine.get_ram_snapshot() if machine and machine.has_method("get_ram_snapshot") else {}
	_single_shapes = Logic.detect_single_screen_shapes(snapshot, settings.single_screen_shapes).duplicate(true)
	var top := Rect2(top_frame.position, top_frame.size)
	for shape in _single_shapes:
		if not shape is Dictionary: continue
		var source: Rect2 = shape.get("source", Rect2())
		var dest := Logic.single_shape_rect(top, shape)
		if source.size.x <= 0 or source.size.y <= 0 or dest.size.x <= 0 or dest.size.y <= 0: continue
		var node := TextureRect.new()
		node.mouse_filter = Control.MOUSE_FILTER_IGNORE
		node.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
		node.stretch_mode = TextureRect.STRETCH_SCALE
		node.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST if settings.nearest else CanvasItem.TEXTURE_FILTER_LINEAR
		node.texture = bottom_rect.texture
		var material := ShaderMaterial.new()
		material.shader = load("res://shaders/single_screen.gdshader")
		material.set_shader_parameter("source_rect", Vector4(source.position.x / 256.0, source.position.y / 192.0, source.size.x / 256.0, source.size.y / 192.0))
		material.set_shader_parameter("linear_filter", not settings.nearest)
		material.set_shader_parameter("use_color_key", shape.has("color_key"))
		if shape.has("color_key"): material.set_shader_parameter("color_key", shape.color_key)
		node.material = material
		node.modulate.a = clampf(float(shape.get("opacity", 1.0)), 0.0, 1.0)
		game_panel.add_child(node)
		node.position = dest.position
		node.size = dest.size
		_single_nodes.append(node)
	_update_single_gpu()

func _update_single_gpu() -> void:
	var enabled := int(settings.renderer_gpu3d) != 0 and not gpu3d_screen_is_top and not _gpu3d_compare_software and _gpu3d_texture != null and _gpu3d_mask != null
	for node in _single_nodes:
		var material := node.material as ShaderMaterial
		material.set_shader_parameter("use_gpu", enabled)
		material.set_shader_parameter("gpu_width_ratio", _gpu_width_ratio())
		if enabled:
			material.set_shader_parameter("gpu_texture", _gpu3d_texture)
			material.set_shader_parameter("gpu_mask", _gpu3d_mask)

func _single_shape_at(point: Vector2) -> int:
	var top := Rect2(Vector2.ZERO, top_rect.size)
	for i in range(_single_shapes.size() - 1, -1, -1):
		var shape = _single_shapes[i]
		if not shape is Dictionary or float(shape.get("opacity", 1.0)) <= 0.0: continue
		var source: Rect2 = shape.get("source", Rect2())
		var dest := Logic.single_shape_rect(top, shape)
		if source.size.x <= 0 or source.size.y <= 0 or not dest.has_point(point): continue
		if shape.has("color_key") and bottom_rect.texture:
			var uv := (source.position + (point - dest.position) / dest.size * source.size) / Vector2(256, 192)
			uv = uv.clamp(Vector2.ZERO, Vector2.ONE)
			var image := bottom_rect.texture.get_image()
			if image:
				var pixel := Vector2i(mini(int(uv.x * image.get_width()), image.get_width() - 1), mini(int(uv.y * image.get_height()), image.get_height() - 1))
				var color := image.get_pixelv(pixel)
				if int(settings.renderer_gpu3d) != 0 and not gpu3d_screen_is_top and not _gpu3d_compare_software and _gpu3d_texture and _gpu3d_mask:
					var mask := _gpu3d_mask.get_image()
					if mask:
						var mask_pixel := Vector2i(mini(int(uv.x * mask.get_width()), mask.get_width() - 1), mini(int(uv.y * mask.get_height()), mask.get_height() - 1))
						if mask.get_pixelv(mask_pixel).r > 0.0:
							var gpu_image := _gpu3d_texture.get_image()
							if gpu_image:
								var gpu_uv := Vector2(0.5 + (uv.x - 0.5) / _gpu_width_ratio(), uv.y)
								var gpu_pixel := Vector2i(mini(int(gpu_uv.x * gpu_image.get_width()), gpu_image.get_width() - 1), mini(int(gpu_uv.y * gpu_image.get_height()), gpu_image.get_height() - 1))
								color = color.blend(gpu_image.get_pixelv(gpu_pixel))
				var key: Color = shape.color_key
				if absf(color.r - key.r) < 0.5 / 255.0 and absf(color.g - key.g) < 0.5 / 255.0 and absf(color.b - key.b) < 0.5 / 255.0: continue
		return i
	return -1

func _on_single_screen_input(event: InputEvent) -> void:
	if layout != 7: return
	var pressed := _touching
	if event is InputEventScreenTouch: pressed = event.pressed
	elif event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT: pressed = event.pressed
	elif not (event is InputEventScreenDrag or event is InputEventMouseMotion): return
	if pressed and not _touching: _single_active = _single_shape_at(event.position)
	if _single_active >= 0 and _single_active < _single_shapes.size():
		var shape: Dictionary = _single_shapes[_single_active]
		var dest := Logic.single_shape_rect(Rect2(Vector2.ZERO, top_rect.size), shape)
		var point := Logic.single_shape_touch(event.position, dest, shape.source)
		if machine: machine.set_touch(point.x, point.y, pressed)
	_touching = pressed and _single_active >= 0
	if not pressed: _single_active = -1
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
func _save_presentation_bundle(dir: String, frame: int) -> String:
	var gpu_stats: Dictionary = machine.get_gpu3d_stats() if machine and machine.has_method("get_gpu3d_stats") else {}
	var requested := int(settings.renderer_gpu3d) != 0
	var enabled := bool(machine.get_gpu3d_enabled()) if machine and machine.has_method("get_gpu3d_enabled") else requested
	var valid := bool(gpu_stats.get("valid", false))
	var composited := enabled and valid and not _gpu3d_compare_software and _gpu3d_texture != null and _gpu3d_mask != null
	var reason := str(gpu_stats.get("fallback", "warmup" if enabled else "disabled"))
	if not enabled: reason = "disabled"
	elif _gpu3d_compare_software: reason = "software_compare"
	elif not composited and reason == "none": reason = "missing_composite_texture"
	var captures := {}
	var textures := {
		"screenshot_window.png": get_viewport().get_texture(),
		"gpu3d_raw.png": machine.get_gpu3d_raw_texture() if machine and machine.has_method("get_gpu3d_raw_texture") else null,
		"gpu3d_output.png": _gpu3d_texture,
		"gpu3d_mask.png": machine.get_gpu3d_raw_mask_texture() if machine and machine.has_method("get_gpu3d_raw_mask_texture") else _gpu3d_mask,
	}
	for filename in textures:
		var texture: Texture2D = textures[filename]
		var image: Image = texture.get_image() if texture else null
		captures[filename] = image != null and not image.is_empty()
		if captures[filename] and image.save_png(dir.path_join(filename)) != OK:
			return "cannot write " + filename
	var window_size := get_window().size
	var viewport_size := get_viewport().get_visible_rect().size
	var bundle_rom: String = replay_issue.env.get("rom_path", settings.rom_path) if not replay_issue.is_empty() else settings.rom_path
	var bundle_save: String = replay_save_path if not replay_save_path.is_empty() else settings.save_path
	var bundle_language: String = replay_issue.language if not replay_issue.is_empty() else settings.language
	var machine_stats: Dictionary = machine.get_stats() if machine else {}
	var rtc_epoch := int(machine.get_rtc_epoch()) if machine and machine.has_method("get_rtc_epoch") else 0
	var rtc_offset := int(machine.get_rtc_timezone_offset_minutes()) if machine and machine.has_method("get_rtc_timezone_offset_minutes") else 0
	var state := {
		"frame": frame,
		"note": pause_note.text,
		"rom_path": ProjectSettings.globalize_path(bundle_rom),
		"save_path": ProjectSettings.globalize_path(bundle_save),
		"language": bundle_language,
		"rtc_epoch": rtc_epoch,
		"rtc_timezone_offset_minutes": rtc_offset,
		"save_at_note": "save_at_note.sav" if FileAccess.file_exists(dir.path_join("save_at_note.sav")) else "",
		"save_at_note_sha256": FileAccess.get_sha256(dir.path_join("save_at_note.sav")) if FileAccess.file_exists(dir.path_join("save_at_note.sav")) else "",
		"machine_stats": machine_stats,
		"widescreen_mode": _active_widescreen_mode,
		"configured_widescreen_mode": int(settings.widescreen_mode),
		"gpu_enabled": enabled,
		"gpu_requested": requested,
		"gpu_composited": composited,
		"gpu_valid": valid,
		"gpu_color_valid": bool(gpu_stats.get("color_valid", false)),
		"gpu_fallback": enabled and not composited,
		"gpu_fallback_reason": reason,
		"gpu_compare_software": _gpu3d_compare_software,
		"gpu_screen": "top" if gpu3d_screen_is_top else "bottom",
		"gpu_scale": int(settings.gpu3d_scale),
		"window_scale": int(settings.window_scale),
		"window_size": [window_size.x, window_size.y],
		"viewport_size": [viewport_size.x, viewport_size.y],
		"layout": layout,
		"integer_scale": bool(settings.integer_scale),
		"nearest": bool(settings.nearest),
		"gpu_stats": gpu_stats,
		"captures": captures,
	}
	return _write_presentation_json(dir.path_join("presentation.json"), state)

func _write_presentation_json(path: String, state: Dictionary) -> String:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return "Cannot open presentation.json for writing at %s: %s" % [path, error_string(FileAccess.get_open_error())]
	var contents := JSON.stringify(state, "\t") + "\n"
	file.store_string(contents)
	var error := file.get_error()
	if error == OK:
		file.flush()
		error = file.get_error()
	file.close()
	if error != OK:
		return "Cannot finish writing presentation.json at %s: %s" % [path, error_string(error)]
	# Some FileAccess backends do not update get_error() after a failed flush.
	if FileAccess.get_file_as_string(path) != contents:
		return "Cannot finish writing presentation.json at %s: written contents could not be read back" % path
	return ""

func _save_bundle() -> void:
	if _bundle_active or not machine: return
	_bundle_active = true
	var captured_machine := machine
	var was_paused: bool = captured_machine.is_paused()
	var gui_was_disabled := get_viewport().gui_disable_input
	get_viewport().gui_disable_input = true
	captured_machine.pause_at_frame_boundary()
	# Render both the GPU subviewports and the window composite after upload.
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	if is_instance_valid(captured_machine) and machine == captured_machine:
		_save_bundle_at_boundary()
		captured_machine.set_paused(was_paused)
	get_viewport().gui_disable_input = gui_was_disabled
	_bundle_active = false

func _save_bundle_at_boundary() -> void:
	var stamp=Time.get_datetime_string_from_system().replace(":","-").replace(" ","_")
	var dir="user://debug/"+stamp
	var frame := _current_frame()
	if machine and machine.has_method("get_input_history"):
		var config := _current_save_config()
		var bundle_rom: String = str(config.get("rom_path", settings.rom_path))
		var bundle_save: String = str(config.get("save_path", settings.save_path))
		var bundle_language: String = str(config.get("language", settings.language))
		var result: String = DebugBundle.save_note(machine, dir, pause_note.text, frame,
			bundle_rom, bundle_save, bundle_language, _active_widescreen_mode)
		if result.is_empty() or result.begins_with("save not written:"):
			_observe_save_result(result.is_empty())
			_queue_save_backup_refresh()
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
	var presentation_error := _save_presentation_bundle(dir, frame)
	if not presentation_error.is_empty():
		push_error(presentation_error)
		return
	var index="user://debug/index.tsv"
	var f=FileAccess.open(index,FileAccess.READ_WRITE if FileAccess.file_exists(index) else FileAccess.WRITE)
	if f:
		f.seek_end()
		f.store_string("%s\t%s\t%s\t%d\n"%[stamp,stamp,pause_note.text.replace("\t"," ").replace("\n"," "),frame])
		f.close()
func _current_frame() -> int:
	return maxi(0, int(machine.get_stats().get("frame", 0)) - 1) if machine else 0

func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST: _quit_app()

func _quit_app() -> void:
	if _closing: return
	_closing = true
	var tree := get_tree()
	_attract_generation += 1
	process_mode = Node.PROCESS_MODE_DISABLED
	_release_machine(true)
	if is_instance_valid(attract):
		attract.free()
		attract = null
	if is_instance_valid(launcher):
		launcher.free()
		launcher = null
	var drain_seconds := maxf(0.1, AudioServer.get_time_to_next_mix() * 2.0)
	tree.create_timer(drain_seconds).timeout.connect(tree.quit)

func _exit_tree() -> void:
	_release_machine(true)
