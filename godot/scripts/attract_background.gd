class_name AttractBackground
extends Node

signal frame_ready(top: Texture2D, bottom: Texture2D)
signal gpu_frame_ready(texture: Texture2D, mask: Texture2D)
signal live_started()
signal live_stopped()
signal released()

const Logic = preload("res://scripts/host_logic.gd")
# No-input EU boot: hold the procedural backdrop until the publisher logos
# have finished. See build/session/LiveAttract/report.md for measurements.
const LOGO_END_FRAME := 1000
const FADE_SECONDS := 0.4

var active := false
var volume := 0.7
var _machine: Node
var _save_path := ""
var _live := false
var _releasing := false
var _fade: Tween
var _logo_hashes: Dictionary = {}
var _show_after := LOGO_END_FRAME
var _last_sample_frame := -15
var _gpu_enabled := true
var _gpu_scale := 3

func set_gpu3d_settings(enabled: bool, scale: int) -> void:
	_gpu_enabled = enabled
	_gpu_scale = clampi(scale, 2, 6)
	gpu_frame_ready.emit(null, null)
	if _machine:
		_machine.set_gpu3d_scale(_gpu_scale)
		_machine.set_gpu3d_enabled(_gpu_enabled)


func start(rom_path: String, abi_config: String, language: String) -> String:
	if active or _releasing:
		return "Attract session is already active or releasing."
	if not ClassDB.class_exists("DqMachine"):
		return "Native extension unavailable."
	var directory := ProjectSettings.globalize_path("user://attract")
	if DirAccess.make_dir_recursive_absolute(directory) != OK:
		return "Cannot create attract save directory."
	# A crashed previous process may have left its disposable save behind.
	# Do not remove another running instance's save.
	var dir := DirAccess.open(directory)
	# OS.is_process_running only accepts children spawned by this instance.
	for filename in dir.get_files():
		if not filename.begins_with("attract-") or not filename.ends_with(".sav"): continue
		var pid := filename.trim_prefix("attract-").trim_suffix(".sav")
		if not pid.is_valid_int(): continue
		var stale := false
		if OS.get_name() == "Linux":
			stale = not DirAccess.dir_exists_absolute("/proc/" + pid)
		elif OS.get_name() == "Windows":
			var output := []
			if OS.execute("tasklist", ["/FI", "PID eq " + pid, "/FO", "CSV", "/NH"], output) == 0:
				stale = not str(output[0]).contains('","' + pid + '",')
		if stale: DirAccess.remove_absolute(directory.path_join(filename))
	_save_path = directory.path_join("attract-%d.sav" % OS.get_process_id())
	DirAccess.remove_absolute(_save_path)
	_machine = ClassDB.instantiate("DqMachine")
	add_child(_machine)
	_machine.set_gpu3d_scale(_gpu_scale)
	_machine.set_gpu3d_enabled(_gpu_enabled)
	_machine.set_backup_type("eeprom64k")
	var previous := OS.get_environment("DQ9_ABI_CONFIG")
	if abi_config.is_empty(): OS.unset_environment("DQ9_ABI_CONFIG")
	else: OS.set_environment("DQ9_ABI_CONFIG", abi_config)
	var error: String = _machine.load_game(ProjectSettings.globalize_path(rom_path), _save_path, Logic.firmware_language(language))
	if previous.is_empty(): OS.unset_environment("DQ9_ABI_CONFIG")
	else: OS.set_environment("DQ9_ABI_CONFIG", previous)
	if not error.is_empty():
		_machine.free()
		_machine = null
		DirAccess.remove_absolute(_save_path)
		return error
	if AudioServer.get_bus_index("Music") < 0:
		AudioServer.add_bus()
		AudioServer.set_bus_name(AudioServer.bus_count - 1, "Music")
	var player: AudioStreamPlayer = _machine.get_audio_player()
	player.bus = "Music"
	player.volume_linear = 0.0
	_logo_hashes.clear()
	_show_after = LOGO_END_FRAME
	_last_sample_frame = -15
	_machine.set_buttons(0)
	_machine.set_touch(0, 0, false)
	_machine.frame_ready.connect(_on_frame)
	_machine.error.connect(_on_error, CONNECT_DEFERRED)
	active = true
	_machine.start()
	return ""

func set_volume(value: float) -> void:
	volume = clampf(value, 0.0, 1.0)
	if _live and not _releasing and _machine:
		_fade_to(volume)

func _fade_to(target: float) -> void:
	if _fade and _fade.is_valid(): _fade.kill()
	if not _machine: return
	_fade = create_tween()
	_fade.tween_property(_machine.get_audio_player(), "volume_linear", target, FADE_SECONDS)

func _on_frame(frame: int) -> void:
	if _releasing or not active: return
	# Stable non-uniform publisher-logo frames are remembered from the first
	# boot. Exact sampled matches hide a repeated logo sequence until its fade.
	# Uniform black/white frames are excluded to avoid ordinary scene fades.
	if frame - _last_sample_frame >= 15 and (frame < 840 or frame >= _show_after):
		_last_sample_frame = frame
		var image: Image = _machine.get_top_image()
		image = image.duplicate()
		image.resize(32, 24, Image.INTERPOLATE_NEAREST)
		var bytes := image.get_data()
		var different := false
		for i in range(4, bytes.size(), 4):
			if bytes[i] != bytes[0] or bytes[i + 1] != bytes[1] or bytes[i + 2] != bytes[2]:
				different = true
				break
		if different:
			var signature := hash(bytes)
			if frame >= 150 and frame < 840:
				_logo_hashes[signature] = true
			elif _logo_hashes.has(signature):
				_show_after = frame + LOGO_END_FRAME
				if _live:
					_live = false
					_fade_to(0.0)
					live_stopped.emit()
	if frame < _show_after: return
	if not _live:
		_live = true
		live_started.emit()
		_fade_to(volume)
	var gpu: Texture2D = _machine.get_gpu3d_texture() if _gpu_enabled else null
	var mask: Texture2D = _machine.get_gpu3d_mask_texture() if _gpu_enabled else null
	if gpu == null or mask == null or not _machine.get_gpu3d_screen_is_top():
		gpu_frame_ready.emit(null, null)
	else:
		gpu_frame_ready.emit(gpu, mask)
	frame_ready.emit(_machine.get_top_texture(), _machine.get_bottom_texture())

func _on_error(message: String) -> void:
	push_warning("Attract session: " + message)
	release()

func release() -> void:
	if _releasing:
		await released
		return
	if not active:
		return
	_releasing = true
	if _live:
		_fade_to(0.0)
		await _fade.finished
	gpu_frame_ready.emit(null, null)
	if _machine:
		_machine.stop()
		_machine.free() # joins the owner and flushes its disposable save
		_machine = null
	DirAccess.remove_absolute(_save_path)
	_save_path = ""
	active = false
	_live = false
	_releasing = false
	live_stopped.emit()
	released.emit()

func _exit_tree() -> void:
	if _fade and _fade.is_valid(): _fade.kill()
	gpu_frame_ready.emit(null, null)
	if _machine:
		_machine.stop()
		_machine.free()
		_machine = null
	if not _save_path.is_empty(): DirAccess.remove_absolute(_save_path)
	active = false
