extends SceneTree

class MachineStub extends Node:
	var enabled := true
	var valid := true
	var image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	var texture := ImageTexture.create_from_image(image)
	var pen_down := false
	func set_touch(_x: int, _y: int, down: bool) -> void: pen_down = down
	func get_stats() -> Dictionary: return {"frame": 18, "fps": 60, "audio_queue_ms": 12, "underruns": 0}
	func get_gpu3d_stats() -> Dictionary: return {"valid": valid, "color_valid": true, "fallback": "none" if valid else "geometry", "source_frame": 16}
	func get_gpu3d_enabled() -> bool: return enabled
	func get_gpu3d_raw_texture() -> Texture2D: return texture
	func get_gpu3d_raw_mask_texture() -> Texture2D: return texture
	func get_rtc_epoch() -> int: return 123
	func get_rtc_timezone_offset_minutes() -> int: return 60

class Host extends "res://scripts/main.gd":
	var bundle_calls := 0
	func _ready() -> void:
		settings = Logic.DEFAULTS.duplicate(true)
		_build_ui()
		settings_panel.hide()
	func _save_bundle() -> void:
		bundle_calls += 1

var ok := true
func expect(condition: bool, message: String) -> void:
	if not condition:
		push_error(message)
		ok = false

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var dir := "user://presentation-bundle-test"
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(dir))
	var app := Host.new()
	root.add_child(app)
	var machine := MachineStub.new()
	app.machine = machine
	app.add_child(machine)
	app.settings.renderer_gpu3d = 1
	app.settings.rom_path = "fixture.nds"
	app.settings.save_path = dir.path_join("source.sav")
	app.settings.language = "fr"
	app._active_widescreen_mode = 2
	app.pause_note.text = "test note"
	app._gpu3d_texture = machine.texture
	app._gpu3d_mask = machine.texture
	var save := FileAccess.open(dir.path_join("save_at_note.sav"), FileAccess.WRITE)
	save.store_buffer(PackedByteArray([1, 2, 3]))
	save.close()
	await process_frame
	var error: String = app._save_presentation_bundle(dir, 17)
	expect(error.is_empty(), "presentation capture: " + error)
	var state: Variant = JSON.parse_string(FileAccess.get_file_as_string(dir.path_join("presentation.json")))
	expect(state is Dictionary, "presentation JSON is a dictionary")
	if state is Dictionary:
		expect(state.get("frame") == 17 and state.get("widescreen_mode") == 2, "frame and active widescreen mode")
		expect(state.get("language") == "fr" and state.get("note") == "test note", "legacy bundle metadata")
		expect(state.get("rtc_epoch") == 123 and state.get("rtc_timezone_offset_minutes") == 60, "RTC metadata")
		expect(state.get("save_at_note_sha256") == FileAccess.get_sha256(dir.path_join("save_at_note.sav")), "save hash")
		expect(state.get("machine_stats", {}).get("fps") == 60 and state.get("gpu_stats", {}).get("source_frame") == 16, "machine and GPU stats")
		expect(state.get("gpu_enabled") and state.get("gpu_valid") and state.get("gpu_composited") and not state.get("gpu_fallback"), "active GPU state")
		for name in ["gpu3d_raw.png", "gpu3d_output.png", "gpu3d_mask.png"]:
			expect(state.get("captures", {}).get(name, false) and FileAccess.file_exists(dir.path_join(name)), "capture " + name)
		expect(bool(state.get("captures", {}).get("screenshot_window.png", false)) == FileAccess.file_exists(dir.path_join("screenshot_window.png")), "window capture availability")
	app._gpu3d_compare_software = true
	error = app._save_presentation_bundle(dir, 17)
	state = JSON.parse_string(FileAccess.get_file_as_string(dir.path_join("presentation.json")))
	expect(error.is_empty() and state.get("gpu_fallback") and state.get("gpu_fallback_reason") == "software_compare" and not state.get("gpu_composited"), "software compare fallback")
	app._gpu3d_compare_software = false
	machine.valid = false
	error = app._save_presentation_bundle(dir, 17)
	state = JSON.parse_string(FileAccess.get_file_as_string(dir.path_join("presentation.json")))
	expect(error.is_empty() and state.get("gpu_fallback") and state.get("gpu_fallback_reason") == "geometry", "producer fallback reason")
	error = app._write_presentation_json(dir, {})
	expect(error.contains("presentation.json") and error.contains(dir), "metadata open failure reports its path")
	var blocked_dir := dir.path_join("blocked")
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(blocked_dir.path_join("presentation.json")))
	error = app._save_presentation_bundle(blocked_dir, 17)
	expect(error.contains("presentation.json") and error.contains(blocked_dir), "bundle reports failed metadata write")
	for name in DirAccess.get_files_at(blocked_dir):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(blocked_dir.path_join(name)))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(blocked_dir.path_join("presentation.json")))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(blocked_dir))
	var key := InputEventKey.new()
	key.keycode = KEY_F9
	key.pressed = true
	app._input(key)
	expect(app.bundle_calls == 1, "F9 dispatches bundle save")
	key.echo = true
	app._input(key)
	expect(app.bundle_calls == 1, "repeated F9 does not save again")
	key.echo = false
	var mouse := InputEventMouseButton.new()
	mouse.button_index = MOUSE_BUTTON_LEFT
	mouse.pressed = true
	mouse.position = Vector2(20, 20)
	app._on_touch_input(mouse)
	expect(app._touching and machine.pen_down, "mouse press starts touch")
	app._single_active = 0
	app._bundle_active = true
	app._input(key)
	expect(app.bundle_calls == 1, "capture suppresses other inputs")
	mouse.pressed = false
	app._input(mouse)
	app._bundle_active = false
	expect(not app._touching and app._single_active == -1 and not machine.pen_down, "mouse release during capture clears touch after resume")
	var touch := InputEventScreenTouch.new()
	touch.pressed = true
	touch.position = Vector2(20, 20)
	app._on_touch_input(touch)
	expect(app._touching and machine.pen_down, "screen press starts touch")
	app._bundle_active = true
	touch.pressed = false
	app._input(touch)
	app._bundle_active = false
	expect(not app._touching and not machine.pen_down, "screen release during capture clears touch after resume")
	app.machine = null
	app.free()
	for name in DirAccess.get_files_at(dir):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(dir.path_join(name)))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(dir))
	print("PASS: presentation bundle" if ok else "FAIL: presentation bundle")
	quit(0 if ok else 1)
