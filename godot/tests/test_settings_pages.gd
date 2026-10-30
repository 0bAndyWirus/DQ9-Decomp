extends SceneTree

# Invoke with a fresh isolated XDG_DATA_HOME: the real Settings overlay persists.
const Logic = preload("res://scripts/host_logic.gd")
const SettingsPages = preload("res://scripts/settings_pages.gd")
const CLASSIC := {"layout": 0, "nearest": true, "integer_scale": false, "renderer_gpu3d": 0, "gpu3d_scale": 3, "widescreen_mode": 0, "hd_textures": false}
const ENHANCED := {"layout": 0, "nearest": true, "integer_scale": false, "renderer_gpu3d": 1, "gpu3d_scale": 3, "widescreen_mode": 1, "hd_textures": false}
const ALTERNATIVES := {"layout": 1, "nearest": false, "integer_scale": true, "gpu3d_scale": 4, "hd_textures": true}
var ok := true

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var settings := {"layout": 1, "nearest": false, "integer_scale": true, "renderer_gpu3d": 1, "gpu3d_scale": 4, "widescreen_mode": 2, "hd_textures": true, "volume": 0.25, "language": "fr"}
	var changed: Dictionary = SettingsPages.apply_preset(settings, 0)
	_expect(changed == CLASSIC, "Classic returns all changed preset values")
	_expect(settings == _with_extras(CLASSIC), "Classic mapping preserves unrelated settings")
	_expect(SettingsPages.apply_preset(settings, 0).is_empty(), "reapplying Classic is a no-op")
	changed = SettingsPages.apply_preset(settings, 1)
	_expect(changed == {"renderer_gpu3d": 1, "widescreen_mode": 1}, "Enhanced returns only changed values")
	_expect(settings == _with_extras(ENHANCED), "Enhanced mapping preserves unrelated settings")
	_expect(SettingsPages.apply_preset(settings, 1).is_empty(), "reapplying Enhanced is a no-op")
	for invalid in [-1, 2, 3]:
		var before := settings.duplicate(true)
		_expect(SettingsPages.apply_preset(settings, invalid).is_empty() and settings == before, "invalid preset %d does not mutate settings" % invalid)
	var empty := {}
	_expect(SettingsPages.apply_preset(empty, 1) == ENHANCED and empty == ENHANCED, "preset fills missing keys")

	var page = SettingsPages.new()
	root.add_child(page)
	page.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	settings = _with_extras(CLASSIC)
	page.configure(settings)
	var tabs: TabContainer = page._controls.tabs
	_expect(tabs.get_tab_count() == 5, "five settings categories")
	for i in 5:
		_expect(tabs.get_tab_title(i) == ["Graphics", "Display", "Audio", "Controls", "Language"][i], "category order %d" % i)
	await process_frame
	await process_frame
	tabs.grab_focus()
	_action("ui_down")
	_expect(root.gui_get_focus_owner() == page._preset, "routed down input enters the current category")
	_action("ui_up")
	_expect(root.gui_get_focus_owner() == tabs, "routed up input returns to categories")
	var motion := InputEventJoypadButton.new()
	motion.button_index = JOY_BUTTON_RIGHT_SHOULDER
	motion.pressed = true
	root.push_input(motion)
	_expect(tabs.current_tab == 1, "routed controller shoulder changes category")
	var next_tab := InputEventKey.new()
	next_tab.keycode = KEY_E
	next_tab.pressed = true
	root.push_input(next_tab)
	_expect(tabs.current_tab == 2, "routed keyboard shortcut changes category")
	tabs.current_tab = 0
	var toggle: CheckButton = page._controls.hd_textures
	var click := InputEventMouseButton.new()
	click.button_index = MOUSE_BUTTON_LEFT
	click.position = toggle.get_global_rect().get_center()
	click.global_position = click.position
	click.pressed = true
	click.button_mask = MOUSE_BUTTON_MASK_LEFT
	root.push_input(click)
	click = click.duplicate()
	click.pressed = false
	click.button_mask = 0
	root.push_input(click)
	_expect(settings.hd_textures, "routed mouse click changes a setting")
	toggle.button_pressed = false
	_expect(tabs.get_node(tabs.focus_neighbor_bottom) == page._preset, "dpad down from tabs reaches first setting")
	_expect(page._preset.get_node(page._preset.focus_neighbor_top) == tabs, "dpad up returns to tabs")
	_expect(page._back.get_node(page._back.focus_neighbor_bottom) == tabs, "dpad down from Back returns to tabs")
	_expect(page._controls.renderer_gpu3d.get_node(page._controls.renderer_gpu3d.focus_neighbor_bottom) != page._controls.gpu3d_scale, "disabled GPU scale is skipped")
	_expect(is_equal_approx(page._controls.menu_music_volume.value, 0.7), "menu music defaults to 70 percent")
	_expect(is_equal_approx(page._controls.ui_sfx_volume.value, 0.7), "UI sounds default to 70 percent")
	_expect(page._controls.launcher_live_background.button_pressed, "live launcher background defaults on")
	_expect(not page._controls.menu_reduced_motion.button_pressed, "reduced motion defaults off")
	var changes := {}
	page.setting_changed.connect(func(key, value): changes[key] = value)
	page._controls.menu_music_volume.value = 0.4
	page._controls.ui_sfx_volume.value = 0.3
	page._controls.launcher_live_background.button_pressed = false
	page._controls.menu_reduced_motion.button_pressed = true
	_expect(is_equal_approx(settings.menu_music_volume, 0.4) and is_equal_approx(changes.menu_music_volume, 0.4), "menu music updates settings and emits")
	_expect(is_equal_approx(settings.ui_sfx_volume, 0.3) and is_equal_approx(changes.ui_sfx_volume, 0.3), "UI sounds update settings and emit")
	_expect(settings.launcher_live_background == false and changes.launcher_live_background == false, "live background updates settings and emits")
	_expect(settings.menu_reduced_motion == true and changes.menu_reduced_motion == true, "reduced motion updates settings and emits")
	_expect(page._controls.menu_music_volume_label.text == "40%", "menu music percentage updates")
	settings.erase("menu_music_volume")
	settings.erase("ui_sfx_volume")
	settings.erase("launcher_live_background")
	settings.erase("menu_reduced_motion")
	page.refresh()
	_expect(is_equal_approx(page._controls.menu_music_volume.value, 0.7) and page._controls.launcher_live_background.button_pressed, "refresh restores defaults without mutating settings")
	_expect(not page._controls.menu_reduced_motion.button_pressed, "refresh restores reduced motion default")
	var shoulder := InputEventJoypadButton.new()
	shoulder.pressed = true
	shoulder.button_index = JOY_BUTTON_LEFT_SHOULDER
	page._input(shoulder)
	_expect(tabs.current_tab == 4, "left shoulder wraps to Language")
	_expect(tabs.get_node(tabs.focus_neighbor_bottom) == page._controls.language, "category change updates first setting navigation")
	shoulder.button_index = JOY_BUTTON_RIGHT_SHOULDER
	page._input(shoulder)
	_expect(tabs.current_tab == 0, "right shoulder wraps to Graphics")
	page._controls.widescreen_mode.focus_entered.emit()
	_expect(page._description.text.contains("Full"), "focused setting has a persistent description")
	_expect(page._wide_badges.text.contains("EXPERIMENTAL") and page._wide_badges.text.contains("RESTART"), "Full warnings are visible")
	page._begin_binding("ds_a")
	page._input(shoulder)
	_expect(tabs.current_tab == 0, "shoulder does not switch categories while rebinding")
	var escape := InputEventKey.new()
	escape.keycode = KEY_ESCAPE
	escape.pressed = true
	_expect(page.handle_rebind(escape), "rebinding consumes Escape")
	_expect(page._binding_action.is_empty(), "Escape cancels rebinding")
	page.refresh()
	_expect(page._preset.selected == 0, "Classic settings classify as Classic")
	page._select_preset(1)
	_expect(settings == _with_extras(ENHANCED) and page._preset.selected == 1, "selecting Enhanced updates live settings and classification")
	page._preset.select(2)
	page._select_preset(2)
	_expect(settings == _with_extras(ENHANCED) and page._preset.selected == 1, "selecting Custom does not change settings")
	page._select_preset(0)
	_expect(settings == _with_extras(CLASSIC) and page._preset.selected == 0, "selecting Classic restores its mapping")
	for preset in [CLASSIC, ENHANCED]:
		var expected_index := 0 if preset == CLASSIC else 1
		for key in preset:
			settings.assign(_with_extras(preset))
			if key == "renderer_gpu3d":
				settings[key] = 1 - int(preset[key])
			elif key == "widescreen_mode":
				settings[key] = 2
			else:
				settings[key] = ALTERNATIVES[key]
			page.refresh()
			_expect(page._preset.selected == 2, "%s change from preset %d classifies as Custom" % [key, expected_index])
			settings[key] = preset[key]
			page.refresh()
			_expect(page._preset.selected == expected_index, "restoring %s returns preset %d" % [key, expected_index])
	settings.clear()
	page.refresh()
	_expect(page._preset.selected == 0, "missing preset keys classify using host defaults")
	page.free()
	var launcher = load("res://scripts/launcher.gd").new()
	root.add_child(launcher)
	launcher.set_continue_enabled(true)
	launcher.show_title()
	for frame in 4: await process_frame
	var title_rect: Rect2 = launcher._title_screen.get_global_rect()
	var title_focus: Control = root.gui_get_focus_owner()
	launcher.hide()
	await process_frame
	launcher.show_title()
	for frame in 4: await process_frame
	_expect(launcher._title_screen.get_global_rect().is_equal_approx(title_rect), "returning to title preserves container layout")
	_expect(root.gui_get_focus_owner() == title_focus, "returning to title restores focus")
	launcher.free()
	await _test_real_modal()
	print("PASS: settings presets and custom classification" if ok else "FAIL: settings presets")
	quit(0 if ok else 1)

func _test_real_modal() -> void:
	if OS.get_environment("XDG_DATA_HOME").is_empty() or FileAccess.file_exists("user://settings.cfg"):
		_expect(false, "real modal test requires a fresh isolated XDG_DATA_HOME")
		return
	var initial: Dictionary = Logic.DEFAULTS.duplicate(true)
	initial.launcher_live_background = false
	initial.menu_reduced_motion = true
	Logic.save_settings("user://settings.cfg", initial)
	var app = load("res://scenes/main.tscn").instantiate()
	root.add_child(app)
	app.launcher.show_title()
	for frame in 4: await process_frame
	for viewport_size in [Vector2i(1280, 800), Vector2i(1920, 1080)]:
		root.size = viewport_size
		for frame in 6: await process_frame
		var launcher = app.launcher
		var title_rect: Rect2 = launcher._title_screen.get_global_rect()
		var heading_rect: Rect2 = launcher._game_title.get_global_rect()
		var buttons: Array[Button] = []
		var button_rects: Array[Rect2] = []
		var settings_button: Button
		for child in launcher._title_screen.get_children():
			if child is Button:
				buttons.append(child)
				button_rects.append(child.get_global_rect())
				if child.text.strip_edges().ends_with("Settings"): settings_button = child
		_expect(settings_button != null, "title Settings button exists at %s" % viewport_size)
		if settings_button == null: continue
		await _click_at(settings_button.get_global_rect().get_center())
		_expect(app.settings_panel.visible, "mouse opens real Settings at %s" % viewport_size)
		if not app.settings_panel.visible: continue
		var pages = app._ui.settings_pages
		var tabs: TabContainer = pages._controls.tabs
		var bar: TabBar = tabs.get_tab_bar()
		var display_rect: Rect2 = bar.get_tab_rect(1)
		await _click_at(bar.get_global_transform() * display_rect.get_center())
		_expect(tabs.current_tab == 1, "mouse selects Display tab through modal at %s" % viewport_size)
		var toggle: CheckButton = pages._controls.nearest
		var previous: bool = app.settings.nearest
		await _click_at(toggle.get_global_rect().get_center())
		_expect(app.settings.nearest != previous and toggle.button_pressed == app.settings.nearest, "mouse toggles setting through modal at %s" % viewport_size)
		await _click_at(pages._back.get_global_rect().get_center())
		_expect(not app.settings_panel.visible and launcher._title_screen.visible, "mouse Back returns to title at %s" % viewport_size)
		for frame in 4: await process_frame
		_expect(launcher._title_screen.get_global_rect().is_equal_approx(title_rect), "title container layout survives Settings at %s" % viewport_size)
		_expect(launcher._game_title.get_global_rect().is_equal_approx(heading_rect), "title heading layout survives Settings at %s" % viewport_size)
		for i in buttons.size():
			_expect(buttons[i].get_global_rect().is_equal_approx(button_rects[i]), "title button %d layout survives Settings at %s" % [i, viewport_size])
		await _click_at(settings_button.get_global_rect().get_center())
		_expect(app.settings_panel.visible, "mouse reopens Settings at %s" % viewport_size)
		var key := InputEventKey.new()
		key.keycode = KEY_E
		key.pressed = true
		var prior_tab: int = tabs.current_tab
		root.push_input(key)
		key = key.duplicate()
		key.pressed = false
		root.push_input(key)
		_expect(tabs.current_tab == (prior_tab + 1) % tabs.get_tab_count(), "keyboard changes tab through modal at %s" % viewport_size)
		key = InputEventKey.new()
		key.keycode = KEY_ESCAPE
		key.pressed = true
		root.push_input(key)
		key = key.duplicate()
		key.pressed = false
		root.push_input(key)
		await process_frame
		_expect(not app.settings_panel.visible and launcher._title_screen.visible, "Escape closes real modal at %s" % viewport_size)
	app.free()

func _click_at(position: Vector2) -> void:
	for pressed in [true, false]:
		var event := InputEventMouseButton.new()
		event.button_index = MOUSE_BUTTON_LEFT
		event.position = position
		event.global_position = position
		event.pressed = pressed
		event.button_mask = MOUSE_BUTTON_MASK_LEFT if pressed else 0
		root.push_input(event)
		await process_frame
	for frame in 3: await process_frame

func _action(name: String) -> void:
	var event := InputEventAction.new()
	event.action = name
	event.pressed = true
	root.push_input(event)
	event = event.duplicate()
	event.pressed = false
	root.push_input(event)

func _with_extras(preset: Dictionary) -> Dictionary:
	var result := preset.duplicate()
	result["volume"] = 0.25
	result["language"] = "fr"
	return result

func _expect(condition: bool, description: String) -> void:
	if not condition: ok = false; push_error(description)
