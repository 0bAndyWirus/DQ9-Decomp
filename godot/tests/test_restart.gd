extends SceneTree
# Restart must release (flush + stop) the old machine synchronously before the new one loads the save.
# xvfb-run -a Godot --path godot --script res://tests/test_restart.gd -- --rom <rom> --save <copy> --autostart
func _init() -> void:
	var scene: Control = load("res://scenes/main.tscn").instantiate()
	root.add_child(scene)
	for i in 120: await process_frame
	var old = scene.machine
	var old_id: int = old.get_instance_id()
	scene._start_game()
	var ok := true
	if is_instance_valid(instance_from_id(old_id)): push_error("old machine still alive when the replacement was created"); ok = false
	var count := 0
	for c in scene.get_children(): if c.get_class() == "DqMachine": count += 1
	if count != 1: push_error("expected exactly 1 DqMachine, found %d" % count); ok = false
	print("PASS: restart" if ok else "FAIL: restart")
	scene._release_machine(false)
	quit(0 if ok else 1)
