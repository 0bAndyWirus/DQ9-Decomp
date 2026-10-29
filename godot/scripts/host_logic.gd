extends RefCounted
class_name HostLogic

const BUTTON_ACTIONS := ["ds_a", "ds_b", "ds_x", "ds_y", "ds_start", "ds_select", "ds_up", "ds_down", "ds_left", "ds_right", "ds_l", "ds_r"]
const BUTTON_BITS := [0, 1, 10, 11, 3, 2, 6, 7, 5, 4, 9, 8]
const BUTTONS := BUTTON_ACTIONS
const LAYOUTS := ["Vertical", "Horizontal", "Top Focus", "Bottom Focus", "Single Top", "Single Bottom", "Merged Overlay", "Single screen"]
const DEFAULTS := {"rom_path":"", "save_path":"user://dq9.sav", "language":"en", "backup_type":"auto", "layout":0, "nearest":true, "integer_scale":false, "window_scale":1, "volume":0.8, "overlay_rect":Rect2(0.58, 0.56, 0.35, 0.35), "single_screen_shapes":[{"source":Rect2(0, 0, 256, 192), "destination":Rect2(0.65, 0.65, 0.35, 0.35), "opacity":1.0}], "renderer_gpu3d":0, "gpu3d_scale":3, "widescreen_mode":0, "hd_textures":false, "issue_show_auto":false}

static func button_mask(pressed_actions: Array) -> int:
	var mask := 0
	for i in BUTTON_ACTIONS.size():
		if BUTTON_ACTIONS[i] in pressed_actions:
			mask |= 1 << BUTTON_BITS[i]
	return mask

static func layout_rects(view: Vector2, layout: int, overlay: Rect2 = Rect2(0.58, 0.56, 0.35, 0.35), integer_scale := false, wide_ratio := 1.0, wide_screen := 0) -> Array[Rect2]:
	var result: Array[Rect2] = [Rect2(), Rect2()]
	var safe := Vector2(maxf(1, view.x), maxf(1, view.y))
	var top_size := Vector2(256, 192)
	var gap := 12.0
	var ratio := maxf(1.0, wide_ratio)
	var side := 128.0 * (ratio - 1.0)
	var scale := minf(safe.x / (256.0 * ratio), safe.y / 396.0)
	if layout == 1:
		scale = minf(safe.x / (524.0 + 2.0 * side), safe.y / 192.0)
	if integer_scale:
		scale = maxf(1.0, floorf(scale))
	match layout:
		0:
			var size := Vector2(256, 396) * scale
			var origin := (safe - size) * 0.5
			result[0] = Rect2(origin, top_size * scale)
			result[1] = Rect2(origin + Vector2(0, 204 * scale), top_size * scale)
		1:
			var size := Vector2(524.0 + 2.0 * side, 192) * scale
			var origin := (safe - size) * 0.5
			result[0] = Rect2(origin + Vector2(side if wide_screen == 0 else 0.0, 0) * scale, top_size * scale)
			result[1] = Rect2(origin + Vector2(268.0 + side * (2.0 if wide_screen == 0 else 1.0), 0) * scale, top_size * scale)
		2, 3:
			var primary_screen := 0 if layout == 2 else 1
			var width_factor := maxf(ratio if wide_screen == primary_screen else 1.0, 0.55 * (ratio if wide_screen != primary_screen else 1.0))
			var big := minf(safe.x / (256.0 * width_factor), (safe.y - gap) / (192.0 * 1.55))
			if integer_scale: big = maxf(1.0, floorf(big))
			var small := maxf(1.0, floorf(big * 0.55)) if integer_scale else big * 0.55
			var origin_y := (safe.y - 192.0 * (big + small) - gap) * 0.5
			var primary := Rect2(Vector2((safe.x - 256.0 * big) * 0.5, origin_y), top_size * big)
			var secondary_size := top_size * small
			var secondary := Rect2(Vector2((safe.x - secondary_size.x) * 0.5, origin_y + 192.0 * big + gap), secondary_size)
			if layout == 2: result[0] = primary; result[1] = secondary
			else: result[1] = primary; result[0] = secondary
		4, 5, 7:
			var visible_screen := 1 if layout == 5 else 0
			var factor := minf(safe.x / (256.0 * (ratio if wide_screen == visible_screen else 1.0)), safe.y / 192.0)
			if integer_scale: factor = maxf(1.0, floorf(factor))
			result[0 if layout != 5 else 1] = Rect2((safe - top_size * factor) * 0.5, top_size * factor)
		6:
			var factor := minf(safe.x / (256.0 * (ratio if wide_screen == 0 else 1.0)), safe.y / 192.0)
			if integer_scale: factor = maxf(1.0, floorf(factor))
			result[0] = Rect2((safe - top_size * factor) * 0.5, top_size * factor)
			var r := overlay
			var bounds := Vector2(clampf(r.size.x, 0.15, 0.75) * safe.x, clampf(r.size.y, 0.15, 0.75) * safe.y)
			var overlay_ratio := ratio if wide_screen == 1 else 1.0
			var overlay_scale := minf(bounds.x / (256.0 * overlay_ratio), bounds.y / 192.0)
			if integer_scale: overlay_scale = maxf(1.0, floorf(overlay_scale))
			var size := top_size * overlay_scale
			var overlay_side := side * overlay_scale if wide_screen == 1 else 0.0
			var pos := Vector2(clampf(r.position.x * safe.x, overlay_side, safe.x - size.x - overlay_side), clampf(r.position.y * safe.y, 0, safe.y - size.y))
			result[1] = Rect2(pos, size)
	return result

# Cartridge save chip names, as the native `--backup-type`; "auto" leaves the choice to the build/save size.
const BACKUP_TYPES := ["auto", "eeprom64k", "eeprom8k", "eeprom512b", "eeprom128k", "flash256k", "flash512k", "flash1m", "flash8m"]
const BACKUP_LABELS := ["Auto", "EEPROM 64 KB", "EEPROM 8 KB", "EEPROM 512 B", "EEPROM 128 KB", "Flash 256 KB", "Flash 512 KB", "Flash 1 MB", "Flash 8 MB"]

# These are UI hints only. The native restore rechecks the actual source and
# loaded cartridge capacity immediately before its atomic replacement.
static func save_restore_error(size: int, capacity: int) -> String:
	if capacity <= 0: return "Save capacity unavailable. Load a ROM before restoring."
	if size != capacity:
		return "Capacity mismatch: backup is %d bytes; the current save chip holds %d bytes. Restore refused; the chip will not change." % [size, capacity]
	return ""

static func save_backup_label(path: String, size: int) -> String:
	var filename := path.get_file()
	var stamp := filename.right(37)
	var date := filename
	if stamp.length() == 37 and stamp.substr(8, 1) == "T" and stamp.ends_with(".bak"):
		date = "%s-%s-%s %s:%s:%s UTC" % [stamp.substr(0, 4), stamp.substr(4, 2), stamp.substr(6, 2), stamp.substr(9, 2), stamp.substr(11, 2), stamp.substr(13, 2)]
	return "%s · %d B" % [date, size]

static func save_backup_rows(backups: Array, capacity: int, maximum := 50) -> Array[Dictionary]:
	var rows: Array[Dictionary] = []
	# Native filenames encode nanoseconds and collision sequence, including clock
	# rollback. Preserve native newest-first order; never re-sort by mtime or label.
	for index in mini(backups.size(), maxi(0, maximum)):
		var backup: Dictionary = backups[index]
		var path: String = backup.path
		var size: int = int(backup.size)
		var error := save_restore_error(size, capacity)
		rows.append({"path": path, "size": size, "label": save_backup_label(path, size), "error": error})
	return rows

# The locally generated runtime configuration: `abi.ini` next to `start`, else `build/gen/abi.ini` in `start` or up to
# five parent folders (the Godot project lives in <repo>/godot). Returns "" when nothing is found.
static func find_abi_config(start: String) -> String:
	var dir := start.simplify_path()
	if dir.is_empty(): return ""
	for depth in 6:
		if depth == 0 and FileAccess.file_exists(dir.path_join("abi.ini")): return dir.path_join("abi.ini")
		var generated := dir.path_join("build/gen/abi.ini")
		if FileAccess.file_exists(generated): return generated
		var parent := dir.get_base_dir()
		if parent == dir or parent.is_empty(): break
		dir = parent
	return ""

# First-run game language from the OS language (the European release ships en/fr/de/es/it).
static func language_for_locale(locale: String) -> String:
	var code := locale.to_lower().split("_")[0].split("-")[0]
	return code if code in ["en", "fr", "de", "es", "it"] else "en"

# DS firmware language ids as used by the SDL frontend: en=1 fr=2 de=3 it=4 es=5 (not the launcher list order).
static func firmware_language(code: String) -> int:
	return {"en": 1, "fr": 2, "de": 3, "it": 4, "es": 5}.get(code.to_lower(), 1)

static func touch_coords(point: Vector2, rect: Rect2) -> Vector2i:
	if rect.size.x <= 0 or rect.size.y <= 0: return Vector2i.ZERO
	var p := (point - rect.position) / rect.size
	return Vector2i(clampi(roundi(clampf(p.x, 0, 1) * 255), 0, 255), clampi(roundi(clampf(p.y, 0, 1) * 191), 0, 191))

# Shape coordinates: source in bottom-screen pixels, destination normalized to
# the displayed top screen. Array order is paint order, last shape on top.
static func single_shape_rect(top: Rect2, shape: Dictionary) -> Rect2:
	var destination: Rect2 = shape.get("destination", Rect2())
	return Rect2(top.position + destination.position * top.size, destination.size * top.size)

static func single_shape_touch(point: Vector2, destination: Rect2, source: Rect2) -> Vector2i:
	if destination.size.x <= 0 or destination.size.y <= 0: return Vector2i.ZERO
	var p := ((point - destination.position) / destination.size).clamp(Vector2.ZERO, Vector2.ONE)
	var pixel := (source.position + p * source.size).floor()
	var last := (source.end - Vector2(0.0001, 0.0001)).floor()
	pixel = Vector2(minf(pixel.x, last.x), minf(pixel.y, last.y))
	return Vector2i(clampi(int(pixel.x), 0, 255), clampi(int(pixel.y), 0, 191))

# No game-specific addresses are assumed until a verified detector is provided.
static func detect_single_screen_shapes(_snapshot: Dictionary, configured: Array) -> Array:
	return configured

static func save_settings(path: String, values: Dictionary) -> void:
	var cfg := ConfigFile.new()
	for key in DEFAULTS: cfg.set_value("settings", key, values.get(key, DEFAULTS[key]))
	for action in BUTTONS:
		var serialized: Array = []
		for event in InputMap.action_get_events(action): serialized.append(event_to_dict(event))
		cfg.set_value("bindings", action, serialized)
	cfg.save(path)

static func load_settings(path: String) -> Dictionary:
	var values := DEFAULTS.duplicate(true)
	var cfg := ConfigFile.new()
	if cfg.load(path) != OK:
		values["language"] = language_for_locale(OS.get_locale())
		return values
	for key in DEFAULTS: values[key] = cfg.get_value("settings", key, DEFAULTS[key])
	values["language"] = values["language"] if values["language"] in ["en", "fr", "de", "es", "it"] else "en"
	values["backup_type"] = values["backup_type"] if values["backup_type"] in BACKUP_TYPES else "auto"
	values["layout"] = clampi(int(values["layout"]), 0, 7)
	if not values["single_screen_shapes"] is Array: values["single_screen_shapes"] = []
	values["renderer_gpu3d"] = clampi(int(values["renderer_gpu3d"]), 0, 1)
	values["gpu3d_scale"] = clampi(int(values["gpu3d_scale"]), 2, 6)
	values["widescreen_mode"] = clampi(int(values["widescreen_mode"]), 0, 2)
	values["hd_textures"] = values["hd_textures"] if values["hd_textures"] is bool else false
	return values

static func event_to_dict(event: InputEvent) -> Dictionary:
	if event is InputEventKey: return {"kind":"key", "code":event.physical_keycode, "logical":event.keycode}
	if event is InputEventJoypadButton: return {"kind":"button", "code":event.button_index}
	if event is InputEventJoypadMotion: return {"kind":"axis", "code":event.axis, "value":event.axis_value}
	return {}

static func event_from_dict(data: Dictionary) -> InputEvent:
	match data.get("kind", ""):
		"key":
			var e := InputEventKey.new(); e.physical_keycode = int(data.get("code", 0)); e.keycode = int(data.get("logical", 0)); return e
		"button":
			var e := InputEventJoypadButton.new(); e.button_index = int(data.get("code", 0)); return e
		"axis":
			var e := InputEventJoypadMotion.new(); e.axis = int(data.get("code", 0)); e.axis_value = float(data.get("value", 0)); return e
	return null

static func apply_bindings(path: String) -> void:
	var cfg := ConfigFile.new()
	if cfg.load(path) != OK: return
	for action in BUTTONS:
		var stored: Array = cfg.get_value("bindings", action, [])
		if stored.is_empty(): continue
		InputMap.action_erase_events(action)
		for item in stored:
			var event := event_from_dict(item)
			if event != null: InputMap.action_add_event(action, event)

static func reset_bindings() -> void:
	var keys := [90, 88, 65, 83, 4194309, 4194308, 4194320, 4194322, 4194319, 4194321, 69, 81]
	var buttons := [1, 0, 3, 2, 6, 4, 11, 12, 13, 14, 9, 10]
	for i in BUTTONS.size():
		InputMap.action_erase_events(BUTTONS[i])
		var key := InputEventKey.new(); key.physical_keycode = keys[i]; InputMap.action_add_event(BUTTONS[i], key)
		var joy := InputEventJoypadButton.new(); joy.button_index = buttons[i]; InputMap.action_add_event(BUTTONS[i], joy)
	if BUTTONS.size() >= 10:
		for index in [6,7,8,9]:
			var motion := InputEventJoypadMotion.new()
			motion.axis = 1 if index < 8 else 0
			motion.axis_value = -1.0 if index in [6,8] else 1.0
			InputMap.action_add_event(BUTTONS[index], motion)
