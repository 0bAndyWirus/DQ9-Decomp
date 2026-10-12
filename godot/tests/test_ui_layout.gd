extends SceneTree
# Regression: in a real window the game screens must be laid out and the pause panel hidden at start.
# Run under a display: xvfb-run -a Godot --path godot --script res://tests/test_ui_layout.gd
func _init() -> void:
	var scene: Control = load("res://scenes/main.tscn").instantiate()
	root.add_child(scene)
	for i in 10: await process_frame
	var ok := true
	if scene.pause_panel.visible: push_error("pause panel visible at start"); ok = false
	for frame in [scene.top_frame, scene.bottom_frame]:
		if not frame.visible or frame.size.x < 100 or frame.size.y < 100:
			push_error("screen frame not laid out: %s" % [frame.size]); ok = false
	print("PASS: ui layout" if ok else "FAIL: ui layout")
	quit(0 if ok else 1)
