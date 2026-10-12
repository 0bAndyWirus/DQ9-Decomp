extends SceneTree
# Routed clicks: Start/Settings button opens settings; clicking the middle of the bottom screen gives DS touch (128,96).
# Needs a display + ROM: xvfb-run -a Godot --path godot --script res://tests/test_ui_clicks.gd -- --rom <rom> --save <copy of a save> --autostart
func _click(pos: Vector2, pressed: bool) -> void:
	var e := InputEventMouseButton.new()
	e.button_index = MOUSE_BUTTON_LEFT; e.pressed = pressed; e.position = pos; e.global_position = pos
	if pressed: e.button_mask = MOUSE_BUTTON_MASK_LEFT
	root.push_input(e)
func _init() -> void:
	var scene: Control = load("res://scenes/main.tscn").instantiate()
	root.add_child(scene)
	for i in 200: await process_frame
	var ok := true
	var rect: Rect2 = scene.bottom_rect.get_global_rect()
	var center := rect.get_center()
	_click(center, true)
	for i in 6: await process_frame
	_click(center, false)
	for i in 6: await process_frame
	var best := Vector2i(-1, -1)
	for row in scene.machine.get_input_history():
		var f := str(row).split(" ")
		if int(f[2]) >= 0: best = Vector2i(int(f[2]), int(f[3]))
	if absi(best.x - 128) > 2 or absi(best.y - 96) > 2:
		push_error("touch at screen center gave %s, expected ~(128,96)" % [best]); ok = false
	var btn: Button = null
	for b in scene.find_children("*", "Button", true, false):
		if b.text == "Start / Settings": btn = b
	_click(btn.get_global_rect().get_center(), true)
	await process_frame
	_click(btn.get_global_rect().get_center(), false)
	for i in 4: await process_frame
	if not scene.settings_panel.visible: push_error("Start / Settings click did not open settings"); ok = false
	print("PASS: ui clicks" if ok else "FAIL: ui clicks")
	scene._release_machine(false)
	quit(0 if ok else 1)
