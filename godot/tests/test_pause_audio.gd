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
	machine.set_paused(true)
	await create_timer(0.2).timeout
	var frozen:int=machine.get_stats().underruns
	await create_timer(0.5).timeout
	var ok:bool=player.stream_paused and machine.get_stats().underruns==frozen
	if not ok: push_error("Paused game must pause audio and not accumulate generator underruns")
	machine.step_once()
	await machine.frame_ready
	await create_timer(0.2).timeout
	if not player.stream_paused or machine.get_stats().underruns!=frozen:
		push_error("Single-frame step must keep audio paused"); ok=false
	machine.set_paused(false)
	await create_timer(0.5).timeout
	if player.stream_paused:
		push_error("Resume must restart retained audio playback"); ok=false
	app._release_machine(false); app.free()
	print("AUDIO_PAUSE ","PASS" if ok else "FAIL")
	quit(0 if ok else 1)
