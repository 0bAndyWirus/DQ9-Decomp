extends SceneTree

# Diagnostic extension build: -DDQ9_AUDIO_TESTS=ON. Captures unresampled
# stereo signed-16 little-endian samples, not the host mixer's float output.
func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() != 3:
		push_error("audio_pcm.gd -- <rom> <fresh-save> <output.pcm>")
		quit(2)
		return
	GDExtensionManager.load_extension("res://dq9.gdextension")
	if OS.get_environment("DQ9_ABI_CONFIG").is_empty():
		var abi: String = preload("res://scripts/host_logic.gd").find_abi_config(ProjectSettings.globalize_path("res://"))
		if not abi.is_empty(): OS.set_environment("DQ9_ABI_CONFIG", abi)
	var machine = ClassDB.instantiate("DqMachine")
	if machine == null or not machine.has_method("set_test_audio_capture"):
		push_error("Requires real DqMachine built with DQ9_AUDIO_TESTS=ON")
		quit(3)
		return
	root.add_child(machine)
	machine.set_rtc_epoch(1700000000, 0)
	machine.set_backup_type("eeprom64k")
	var error: String = machine.load_game(args[0], args[1], 1)
	if not error.is_empty():
		push_error(error)
		machine.free()
		quit(4)
		return
	machine.set_test_audio_capture(true)
	machine.step_frames(3000, 0, -1, -1, false)
	var pcm: PackedByteArray = machine.take_test_audio()
	var file := FileAccess.open(args[2], FileAccess.WRITE)
	if file == null:
		push_error("Cannot open PCM destination")
		machine.free()
		quit(5)
		return
	file.store_buffer(pcm)
	file.close()
	var ok: bool = machine.flush_save()
	print("SOURCE_PCM frames=", machine.get_stats().frame, " bytes=", pcm.size())
	machine.free()
	await create_timer(0.5).timeout
	quit(0 if ok else 6)
