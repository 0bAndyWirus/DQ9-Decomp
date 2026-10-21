extends SceneTree

# Real-extension realtime probe. No game data is written into the source tree.
var machine: Node
var began := 0
var interval_start := 0
var interval_frame := 0
var host_times: Array[float] = []
var main_times: Array[float] = []
var phase := 0
var mode := "normal"
var finish_frame := 3000
var max_ring := 0.0
var max_generator := 0.0
var max_total := 0.0
var generator_capacity := 0
var release_ms := 0
var failed := false
var paused_underruns := -1

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() < 2:
		push_error("audio_pass.gd -- <rom> <fresh-save> [normal|fast-forward|pause]")
		quit(2)
		return
	if args.size() > 2: mode = args[2]
	if OS.get_environment("DQ9_ABI_CONFIG").is_empty():
		var abi: String = preload("res://scripts/host_logic.gd").find_abi_config(ProjectSettings.globalize_path("res://"))
		if not abi.is_empty(): OS.set_environment("DQ9_ABI_CONFIG", abi)
	if GDExtensionManager.load_extension("res://dq9.gdextension") not in [OK, ERR_ALREADY_IN_USE]:
		quit(3)
		return
	machine = ClassDB.instantiate("DqMachine")
	root.add_child(machine)
	machine.set_rtc_epoch(1700000000, 0)
	machine.set_backup_type("eeprom64k")
	var error: String = machine.load_game(args[0], args[1], 1)
	if not error.is_empty():
		push_error(error)
		quit(4)
		return
	var screens := VBoxContainer.new()
	root.add_child(screens)
	for texture in [machine.get_top_texture(), machine.get_bottom_texture()]:
		var screen := TextureRect.new()
		screen.texture = texture
		screens.add_child(screen)
	generator_capacity = machine.get_audio_player().get_stream_playback().get_frames_available()
	began = Time.get_ticks_msec()
	interval_start = began
	machine.start()
	print("second,frame,fps,ring_ms,generator_ms,underruns,dropped_ms,host_avg_ms,host_max_ms,main_avg_ms,main_max_ms")

func _process(delta: float) -> bool:
	if machine == null: return false
	var stats: Dictionary = machine.get_stats()
	var frame := int(stats.frame)
	var now := Time.get_ticks_msec()
	host_times.append(delta * 1000.0)
	main_times.append(Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0)
	var playback = machine.get_audio_player().get_stream_playback()
	var generator_ms: float = 1000.0 * (generator_capacity - playback.get_frames_available()) / AudioServer.get_mix_rate()
	max_ring = maxf(max_ring, float(stats.audio_queue_ms))
	max_generator = maxf(max_generator, generator_ms)
	max_total = maxf(max_total, float(stats.audio_queue_ms) + generator_ms)
	if mode == "fast-forward" and frame >= 1000 and phase == 0:
		machine.set_speed(0.0)
		phase = 1
	if mode == "fast-forward" and frame >= 2000 and phase == 1:
		machine.set_speed(1.0)
		release_ms = now
		phase = 2
		print("RELEASE_BEFORE ", stats)
		var after: Dictionary = machine.get_stats()
		print("RELEASE_AFTER ", after)
		if float(after.audio_queue_ms) >= 60.0 or float(after.get("fast_forward_discarded_ms", 0.0)) <= 0.0:
			push_error("Fast-forward release must discard the stale source backlog")
			failed = true
	if mode == "pause" and frame >= 1000 and phase == 0:
		machine.set_paused(true)
		release_ms = now + 1000
		phase = 1
		paused_underruns = int(stats.underruns)
	if mode == "pause" and phase == 1 and now >= release_ms:
		if int(stats.underruns) != paused_underruns:
			push_error("Paused mixer accumulated underruns")
			failed = true
		machine.set_paused(false)
		phase = 2
		print("RESUME ", stats)
	if now - interval_start >= 1000:
		print("%.3f,%d,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.3f" % [
			(now-began)/1000.0, frame, (frame-interval_frame)*1000.0/(now-interval_start),
			stats.audio_queue_ms, generator_ms, stats.underruns, stats.dropped_audio_ms,
			_average(host_times), host_times.max(), _average(main_times), main_times.max()])
		host_times.clear()
		main_times.clear()
		interval_frame = frame
		interval_start = now
	if frame >= finish_frame:
		machine.stop()
		machine.set_paused(true)
		var ok: bool = machine.flush_save()
		print("FINAL elapsed_ms=", now-began, " max_ring_ms=", max_ring, " max_generator_ms=", max_generator, " max_total_ms=", max_total, " stats=", machine.get_stats())
		playback = null
		machine.free()
		machine = null
		call_deferred("_finish", ok and not failed)
	return false

func _finish(ok: bool) -> void:
	# SceneTree._process must return bool immediately. Awaiting there makes its
	# coroutine state truthy, exits early with status zero and skips mixer cleanup.
	await create_timer(2.0).timeout
	quit(0 if ok else 5)

func _average(values: Array[float]) -> float:
	var total := 0.0
	for value in values: total += value
	return total / max(1, values.size())
