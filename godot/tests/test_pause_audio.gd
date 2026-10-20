extends SceneTree
# Requires native extension and --rom/--save/--autostart launcher arguments.
func _initialize() -> void: call_deferred("_run")
func _run() -> void:
	var app=load("res://scenes/main.tscn").instantiate()
	root.add_child(app)
	if app.machine==null: push_error("Native autostart required"); quit(2); return
	await create_timer(2).timeout
	var machine=app.machine
	var player: AudioStreamPlayer=machine.get_audio_player()
	var ok := true
	for cycle in range(10):
		machine.set_paused(true)
		await create_timer(0.2).timeout
		var frozen: int = machine.get_stats().underruns
		await create_timer(0.1).timeout
		if not player.stream_paused or machine.get_stats().underruns != frozen:
			push_error("Paused game must pause audio and not accumulate generator underruns")
			ok = false
		machine.step_once()
		await machine.frame_ready
		await create_timer(0.05).timeout
		if not player.stream_paused or machine.get_stats().underruns != frozen:
			push_error("Single-frame step must keep audio paused")
			ok = false
		machine.set_paused(false)
		await create_timer(0.25).timeout
		if player.stream_paused or machine.get_stats().underruns != frozen:
			push_error("Resume must re-prime retained audio without adding an underrun")
			ok = false
	app._release_machine(false); app.free()
	print("AUDIO_PAUSE ","PASS" if ok else "FAIL")
	player = null
	machine = null
	await create_timer(2.0).timeout
	quit(0 if ok else 1)
