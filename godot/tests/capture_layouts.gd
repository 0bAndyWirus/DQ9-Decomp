extends SceneTree

## Run from the project root, for example:
## godot --path godot --script res://tests/capture_layouts.gd -- --rom /local/game.nds --save /local/qa.sav --output-dir ../build/godot-captures
## Use a graphical/offscreen rendering backend (not --headless with the dummy renderer):
## viewport texture readback is required to save valid PNGs. A display server such as
## Xvfb may be used with Godot's normal GL compatibility renderer.
const LAYOUT_NAMES := [
	"vertical",
	"horizontal",
	"top_focus",
	"bottom_focus",
	"single_top",
	"single_bottom",
	"merged_overlay",
]

var _root: Control
var _output_dir := ""

func _initialize() -> void:
	call_deferred("_capture")

func _capture() -> void:
	_output_dir = _output_directory()
	var absolute_dir := ProjectSettings.globalize_path(_output_dir)
	var mkdir_error := DirAccess.make_dir_recursive_absolute(absolute_dir)
	if mkdir_error != OK:
		_fail("Cannot create capture directory %s (error %d)" % [absolute_dir, mkdir_error])
		return

	var packed := load("res://scenes/main.tscn") as PackedScene
	if packed == null:
		_fail("Could not load res://scenes/main.tscn")
		return
	_root = packed.instantiate() as Control
	if _root == null:
		_fail("main.tscn root is not a Control")
		return
	get_root().add_child(_root)
	await process_frame
	await process_frame
	if not _has_capture_api():
		return

	# Capture the real extension using --rom / --save launcher arguments.
	if _root.machine == null: _root._start_game()
	if _root.machine == null:
		_fail("Capture requires the native extension and a valid --rom")
		return
	await process_frame
	await process_frame

	_root.settings_panel.hide()
	_root.pause_panel.hide()
	for index in LAYOUT_NAMES.size():
		_root._set_layout(index)
		await process_frame
		await process_frame
		if not await _save_viewport(LAYOUT_NAMES[index]):
			return

	# Settings overlay is shown against the default balanced layout.
	_root._set_layout(0)
	_root._show_settings()
	_root.pause_panel.hide()
	await process_frame
	await process_frame
	if not await _save_viewport("settings"):
		return

	# Hide settings and invoke the existing pause controller with a live stub.
	_root._hide_settings()
	_root._toggle_pause()
	await process_frame
	await process_frame
	if not _root.pause_panel.visible:
		_fail("Pause controller did not show pause_panel")
		return
	if not await _save_viewport("pause"):
		return

	print("Saved seven layouts plus settings and pause PNGs under %s" % absolute_dir)
	quit(0)

func _output_directory() -> String:
	var args := OS.get_cmdline_user_args()
	for i in args.size():
		if args[i] == "--output-dir" and i + 1 < args.size():
			var requested := args[i + 1]
			return requested if requested.begins_with("res://") or requested.begins_with("user://") or requested.is_absolute_path() else "res://" + requested
	return "res://../build/godot-captures"

func _has_capture_api() -> bool:
	for property in ["settings", "settings_panel", "pause_panel"]:
		if not _has_property(_root, property):
			_fail("main.gd does not expose required capture property '%s'" % property)
			return false
	for method in ["_set_layout", "_start_game", "_show_settings", "_hide_settings", "_toggle_pause"]:
		if not _root.has_method(method):
			_fail("main.gd does not expose required capture method '%s'" % method)
			return false
	if _root.settings_panel == null or _root.pause_panel == null:
		_fail("Settings or pause panel was not constructed")
		return false
	return true

func _has_property(object: Object, property_name: String) -> bool:
	for property in object.get_property_list():
		if property.name == property_name:
			return true
	return false

func _save_viewport(name: String) -> bool:
	var image := get_root().get_texture().get_image()
	if image == null or image.is_empty():
		_fail("Viewport image readback is empty for '%s'; use a non-dummy graphical/offscreen renderer" % name)
		return false
	var path := _output_dir.path_join(name + ".png")
	var error := image.save_png(ProjectSettings.globalize_path(path))
	if error != OK:
		_fail("Could not save %s (error %d)" % [path, error])
		return false
	print("Captured %s" % path)
	return true

func _fail(message: String) -> void:
	push_error(message)
	quit(1)
