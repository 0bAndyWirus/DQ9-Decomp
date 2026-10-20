extends SceneTree

const Logic = preload("res://scripts/host_logic.gd")
const DebugBundle = preload("res://scripts/debug_bundle.gd")

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() != 5:
		push_error("usage: Godot --headless --path godot --script res://tests/capture_replay.gd -- <rom> <save-copy> <input-script> <bundle-dir> <frames>")
		quit(2)
		return
	var events: Dictionary = {}
	var first_event := -1
	var script := FileAccess.open(args[2], FileAccess.READ)
	if script == null:
		push_error("cannot open input script")
		quit(2)
		return
	while not script.eof_reached():
		var parts := script.get_line().strip_edges().split(" ", false)
		if parts.size() == 4:
			var absolute_frame := int(parts[0])
			if first_event < 0: first_event = absolute_frame
			events[absolute_frame - first_event] = [int(parts[1]), int(parts[2]), int(parts[3])]
			if absolute_frame >= first_event + int(args[4]): break
	script.close()
	if first_event < 0:
		push_error("input script is empty")
		quit(2)
		return
	if not ClassDB.class_exists("DqMachine"):
		GDExtensionManager.load_extension("res://dq9.gdextension")
	var machine = ClassDB.instantiate("DqMachine")
	if machine == null:
		push_error("DqMachine GDExtension class is unavailable")
		quit(3)
		return
	root.add_child(machine)
	machine.set_rtc_epoch(0, 0)
	var error: String = machine.load_game(args[0], args[1], 1)
	if not error.is_empty():
		push_error(error)
		machine.free()
		quit(4)
		return
	machine.set_paused(true)
	for frame in range(int(args[4])):
		var state: Array = events.get(frame, [0, -1, -1])
		var actions: Array[String] = []
		for i in Logic.BUTTON_BITS.size():
			if int(state[0]) & (1 << Logic.BUTTON_BITS[i]):
				actions.append(Logic.BUTTON_ACTIONS[i])
		var mask := Logic.button_mask(actions)
		var x: int = int(state[1])
		var y: int = int(state[2])
		machine.step_frames(1, mask, x, y, x >= 0 and y >= 0)
		await machine.frame_ready
	var frame := maxi(0, int(machine.get_stats()["frame"]) - 1)
	var result := DebugBundle.save_note(machine, args[3], "Godot scripted replay test", frame,
		args[0], args[1], "en")
	if not result.is_empty():
		push_error(result)
		machine.stop()
		machine.free()
		quit(5)
		return
	print("Captured %d scripted frames; screenshot frame=%d; action input begins at script frame %d" % [int(args[4]), frame, first_event])
	machine.stop()
	machine.call_deferred("free")
	machine = null
	await process_frame
	await create_timer(0.5).timeout
	quit(0)
