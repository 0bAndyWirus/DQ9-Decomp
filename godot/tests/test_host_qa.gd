extends SceneTree

const Logic = preload("res://scripts/host_logic.gd")
var app
var failures: Array[String] = []

func check(value: bool, message: String) -> void:
	if not value:
		failures.append(message)
		push_error(message)

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	app = load("res://scenes/main.tscn").instantiate()
	root.add_child(app)
	await process_frame
	await process_frame
	check(ClassDB.class_exists("DqMachine"), "Scene must load the native extension without an editor cache")
	check(app.launcher.visible and not app.settings_panel.visible, "Launcher must be visible without --autostart")
	var toast_token: int = app._toast_token
	app._observe_save_result(false)
	check(app._toast.visible and app._toast.text.begins_with("SAVE NOT WRITTEN"), "Save failure must be visible")
	check(app._toast_token == toast_token + 1, "First failure shows one toast")
	app._observe_save_result(false)
	check(app._toast_token == toast_token + 1, "Repeated failures must not restart the toast")
	app._observe_save_result(true)
	check(app._toast.text == "SAVE WRITTEN: SAVE PATH WORKING AGAIN" and app._toast_token == toast_token + 2, "Recovery is shown once")
	app._observe_save_result(true)
	check(app._toast_token == toast_token + 2, "Repeated successes must not show recovery again")
	app._observe_save_result(false)
	check(app._toast_token == toast_token + 3, "A new failure after recovery is visible")
	app._observe_save_result(true)
	app._show_toast("")
	app.launcher.settings_requested.emit()
	check(app.settings_panel.visible, "Launcher settings request must open settings")
	var pages = app._ui.settings_pages
	var joy := InputEventJoypadButton.new()
	joy.button_index = JOY_BUTTON_A
	joy.pressed = true
	pages._binding_buttons["ds_a"].pressed.emit()
	Input.parse_input_event(joy)
	await process_frame
	check(pages._binding_action.is_empty(), "Joypad button must finish rebinding")
	check(InputMap.action_has_event("ds_a", joy), "Joypad binding must be installed")
	Logic.reset_bindings()
	app.settings.rom_path = "/nonexistent/qa-missing.nds"
	app._start_game()
	await process_frame
	check(app.machine == null, "Invalid ROM must not leave a machine")
	check(app.launcher.visible and app.launcher._wizard.visible and not app.launcher._status.text.is_empty(), "Invalid ROM needs a visible launcher error message")
	app._hide_settings()
	for size in [Vector2(1280,800), Vector2(3072,1920)]:
		for layout in 7:
			var rects := Logic.layout_rects(size, layout)
			for rect in rects:
				if rect.size == Vector2.ZERO: continue
				check(absf(rect.size.x / rect.size.y - 4.0/3.0) < 0.001, "DS aspect ratio layout %d" % layout)
				check(Rect2(Vector2.ZERO,size).encloses(rect), "Layout %d fits viewport" % layout)
		for layout in [0,1,2,3,4,5,6]:
			for rect in Logic.layout_rects(size,layout,Logic.DEFAULTS.overlay_rect,true):
				if rect.size == Vector2.ZERO: continue
				check(is_equal_approx(rect.size.x/256.0, floorf(rect.size.x/256.0)), "Integer scale applies to every DS screen")
	# The returned rectangles remain native 4:3 touch/2D surfaces, while
	# the GPU child may extend equally to either side of engine A.
	for size in [Vector2(640, 900), Vector2(1280, 600)]:
		for layout in 8:
			for wide_screen in 2:
				for integer in [false, true]:
					var rects := Logic.layout_rects(size, layout, Rect2(0, 0, 0.75, 0.75), integer, 4.0 / 3.0, wide_screen)
					for screen in 2:
						var rect: Rect2 = rects[screen]
						if rect.size == Vector2.ZERO: continue
						check(absf(rect.size.x / rect.size.y - 4.0 / 3.0) < 0.001, "Wide layout %d keeps native screen aspect" % layout)
						var side := rect.size.x / 6.0 if screen == wide_screen else 0.0
						check(rect.position.x - side >= -0.01 and rect.end.x + side <= size.x + 0.01, "Wide layout %d screen %d fits viewport" % [layout, screen])
						if screen == 1:
							check(Logic.touch_coords(rect.position, rect) == Vector2i.ZERO and Logic.touch_coords(rect.end, rect) == Vector2i(255, 191), "Wide layout %d preserves touch coordinates" % layout)
	for layout in 7:
		app._set_layout(layout)
		await process_frame
		await process_frame
		for texture in [app.top_rect,app.bottom_rect]:
			if not texture.is_visible_in_tree(): continue
			check(absf(texture.size.x/texture.size.y-4.0/3.0)<0.001, "Actual DS texture aspect layout %d"%layout)
	app._overlay_rect=Logic.DEFAULTS.overlay_rect
	var initial: Rect2 = app._overlay_rect
	var press := InputEventMouseButton.new()
	press.button_index=MOUSE_BUTTON_LEFT; press.pressed=true; press.alt_pressed=true; press.global_position=Vector2(100,100)
	app._on_touch_input(press)
	var motion := InputEventMouseMotion.new()
	motion.button_mask=MOUSE_BUTTON_MASK_LEFT; motion.global_position=Vector2(150,150)
	app._on_touch_input(motion)
	check(app._overlay_rect.position!=initial.position,"Overlay drag uses event position, not newest global mouse state")
	press.pressed=false; app._input(press)
	check(not app._overlay_drag,"Overlay release outside screen ends gesture")
	press.button_index=MOUSE_BUTTON_RIGHT; press.pressed=true; press.global_position=Vector2(100,100)
	app._on_touch_input(press)
	motion.button_mask=MOUSE_BUTTON_MASK_RIGHT; motion.global_position=Vector2(150,150)
	app._on_touch_input(motion)
	check(app._overlay_rect.size!=initial.size,"Overlay resize uses event position")
	press.pressed=false; app._input(press)
	press.button_index=MOUSE_BUTTON_LEFT; press.pressed=true; press.alt_pressed=false
	app._on_touch_input(press)
	check(app._touching and not app._overlay_drag,"Overlay ordinary click is DS touch, not drag")
	press.pressed=false; app._input(press)
	check(not app._touching,"Touch release outside screen ends pen contact")
	var scale: OptionButton = pages._controls.window_scale
	scale.item_selected.emit(0)
	await process_frame
	await process_frame
	scale.item_selected.emit(1)
	await process_frame
	await process_frame
	check(root.size==Vector2i(2560,1600) and root.get_texture().get_width()==2560,"Window scale control updates Window and viewport, not only DisplayServer")
	scale.item_selected.emit(0)
	await process_frame
	await process_frame
	var fullscreen:=InputEventKey.new(); fullscreen.keycode=KEY_F11; fullscreen.pressed=true
	app._input(fullscreen)
	await process_frame
	check(root.mode==Window.MODE_FULLSCREEN,"F11 updates the owning Window mode (got %d)"%root.mode)
	app._input(fullscreen)
	root.size=Vector2i(3072,1920)
	await process_frame
	await process_frame
	check(root.get_texture().get_width()==3072 and root.get_texture().get_height()==1920,"Physical window resize updates viewport")
	app.free()
	print("HOST_QA failures=%d" % failures.size())
	quit(0 if failures.is_empty() else 1)
