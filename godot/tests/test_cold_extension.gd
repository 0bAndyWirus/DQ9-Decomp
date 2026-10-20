extends SceneTree
# Run in a disposable project with no .godot cache; no image or save required.
func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	if ClassDB.class_exists("DqMachine"):
		push_error("Cold check requires fresh extension registration")
		quit(1)
		return
	var status := GDExtensionManager.load_extension("res://dq9.gdextension")
	if status != GDExtensionManager.LOAD_STATUS_OK or not ClassDB.class_exists("DqMachine"):
		push_error("Cold extension load failed (status %d)" % status)
		quit(1)
		return
	var machine = ClassDB.instantiate("DqMachine")
	if machine == null:
		push_error("DqMachine could not be created")
		quit(1)
		return
	root.add_child(machine)
	await process_frame
	if not machine.get_top_image() is Image or not machine.get_bottom_image() is Image:
		push_error("DqMachine image bindings are unavailable")
		machine.free()
		quit(1)
		return
	machine.free()
	print("PASS: cold-cache extension loaded; DqMachine created; image bindings available")
	quit(0)
