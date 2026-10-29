extends SceneTree
const Bundle = preload("res://scripts/debug_bundle.gd")

class MachineStub extends Node:
	var image := Image.create(256, 192, false, Image.FORMAT_RGBA8)
	func get_stats() -> Dictionary: return {}
	func get_top_image() -> Image: return image
	func get_bottom_image() -> Image: return image
	func get_input_history() -> PackedStringArray: return PackedStringArray(["0 0 -1 -1"])
	func get_session_start_save() -> PackedByteArray: return PackedByteArray([1, 2])
	func flush_save() -> bool: return true
	func get_rtc_epoch() -> int: return 0
	func get_rtc_timezone_offset_minutes() -> int: return 0

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var base := "user://widescreen-bundle-test"
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(base))
	var save := base.path_join("save.sav")
	var f := FileAccess.open(save, FileAccess.WRITE)
	f.store_buffer(PackedByteArray([3, 4])); f.close()
	var machine := MachineStub.new()
	var ok := true
	for mode in [0, 1, 2]:
		var dir := base.path_join(str(mode))
		var error := Bundle.save_note(machine, dir, "test", 0, "fixture.nds", save, "en", mode)
		if not error.is_empty():
			push_error(error); ok = false; continue
		var env := FileAccess.get_file_as_string(dir.path_join("env.txt"))
		var script := FileAccess.get_file_as_string(dir.path_join("replay.sh"))
		ok = ok and env.contains("widescreen_mode: %d\n" % mode)
		if mode == 2:
			var output: Array = []
			var status := OS.execute("/bin/sh", [ProjectSettings.globalize_path(dir.path_join("replay.sh"))], output, true)
			ok = ok and status != 0 and str(output).contains("Full widescreen") and not script.contains("exec ")
		else:
			ok = ok and script.contains("exec ")
		for name in DirAccess.get_files_at(dir): DirAccess.remove_absolute(ProjectSettings.globalize_path(dir.path_join(name)))
		DirAccess.remove_absolute(ProjectSettings.globalize_path(dir))
	machine.free()
	DirAccess.remove_absolute(ProjectSettings.globalize_path(save))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(base))
	if not ok: push_error("widescreen bundle mode regression")
	print("PASS: widescreen bundle" if ok else "FAIL: widescreen bundle")
	quit(0 if ok else 1)
