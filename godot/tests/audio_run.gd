extends SceneTree

const DURATION_SECONDS := 90.0
const FRAME_RATE := 59.8261

var machine: Node
var input_by_frame: Dictionary = {}
var last_stats: Dictionary = {}
var sample_elapsed := 0.0
var bucket_elapsed := 0.0
var bucket_frames := 0
var bucket_ring: Array[float] = []
var bucket_generator: Array[float] = []
var bucket_host_ms: Array[float] = []
var bucket_skips := 0
var started_ms := 0
var generator_capacity_frames := -1
var previous_sample_ms := 0

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() != 3:
		push_error("usage: Godot --headless --path godot --script res://tests/audio_run.gd -- <rom> <save-copy> <input-script>")
		quit(2)
		return
	var input_file := FileAccess.open(args[2], FileAccess.READ)
	if input_file == null:
		push_error("cannot open input script: " + args[2])
		quit(2)
		return
	while not input_file.eof_reached():
		var parts := input_file.get_line().strip_edges().split(" ", false)
		if parts.size() == 4:
			input_by_frame[int(parts[0])] = [int(parts[1]), int(parts[2]), int(parts[3])]
	input_file.close()
	machine = ClassDB.instantiate("DqMachine")
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
	machine.set_speed(1.0)
	started_ms = Time.get_ticks_msec()
	previous_sample_ms = started_ms
	machine.start()
	print("second,emulated_fps,ring_fill_ms,generator_queue_min_ms,generator_queue_avg_ms,generator_queue_max_ms,skips,host_frame_ms_avg,host_frame_ms_max")
	await process_frame

func _process(delta: float) -> bool:
	if machine == null:
		return false
	var stats: Dictionary = machine.get_stats()
	var frame := int(stats.get("frame", 0))
	var state: Array = input_by_frame.get(frame, [0, -1, -1])
	machine.set_buttons(int(state[0]))
	if int(state[1]) >= 0 and int(state[2]) >= 0:
		machine.set_touch(int(state[1]), int(state[2]), true)
	else:
		machine.set_touch(0, 0, false)
	bucket_frames = frame
	bucket_ring.append(float(stats.get("audio_queue_ms", 0.0)))
	var player = machine.get_audio_player()
	var playback = player.get_stream_playback() if player != null else null
	var generator_ms := 0.0
	if playback != null:
		if generator_capacity_frames < 0:
			generator_capacity_frames = playback.get_frames_available()
		generator_ms = 1000.0 * float(generator_capacity_frames - playback.get_frames_available()) / AudioServer.get_mix_rate()
	bucket_generator.append(generator_ms)
	var host_ms := delta * 1000.0
	bucket_host_ms.append(host_ms)
	var now := Time.get_ticks_msec()
	bucket_elapsed = float(now - started_ms) / 1000.0
	if now - previous_sample_ms >= 1000:
		var elapsed := float(now - previous_sample_ms) / 1000.0
		var fps := float(bucket_frames - int(last_stats.get("frame", 0))) / elapsed
		var skips := int(stats.get("underruns", 0))
		var previous_skips := int(last_stats.get("underruns", 0))
		print("%d,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%.2f,%.2f" % [
			int(bucket_elapsed), fps, _average(bucket_ring), _minimum(bucket_generator),
			_average(bucket_generator), _maximum(bucket_generator), skips - previous_skips,
			_average(bucket_host_ms), _maximum(bucket_host_ms)])
		bucket_ring.clear()
		bucket_generator.clear()
		bucket_host_ms.clear()
		previous_sample_ms = now
		last_stats = stats.duplicate()
	if bucket_elapsed >= DURATION_SECONDS:
		machine.stop()
		machine.set_paused(true)
		machine.flush_save()
		print("FINAL ", machine.get_stats())
		if player != null:
			player.stop()
			player.set_stream(null)
		playback = null
		player = null
		machine.free()
		machine = null
		await process_frame
		var settle_timer := Timer.new()
		settle_timer.one_shot = true
		settle_timer.wait_time = 2.0
		root.add_child(settle_timer)
		settle_timer.start()
		await settle_timer.timeout
		settle_timer.stop()
		settle_timer.free()
		settle_timer = null
		await process_frame
		await process_frame
		await process_frame
		call_deferred("quit", 0)
	return false
func _average(values: Array[float]) -> float:
	if values.is_empty():
		return 0.0
	var total := 0.0
	for value in values:
		total += value
	return total / values.size()

func _minimum(values: Array[float]) -> float:
	if values.is_empty():
		return 0.0
	var result := values[0]
	for value in values:
		result = minf(result, value)
	return result

func _maximum(values: Array[float]) -> float:
	if values.is_empty():
		return 0.0
	var result := values[0]
	for value in values:
		result = maxf(result, value)
	return result
