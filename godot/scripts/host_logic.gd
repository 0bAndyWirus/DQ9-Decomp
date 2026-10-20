extends RefCounted
class_name HostLogic

const BUTTON_ACTIONS := ["ds_a", "ds_b", "ds_x", "ds_y", "ds_start", "ds_select", "ds_up", "ds_down", "ds_left", "ds_right", "ds_l", "ds_r"]
const BUTTON_BITS := [0, 1, 10, 11, 3, 2, 6, 7, 5, 4, 9, 8]
const BUTTONS := BUTTON_ACTIONS
const LAYOUTS := ["Vertical", "Horizontal", "Top Focus", "Bottom Focus", "Single Top", "Single Bottom", "Merged Overlay"]
const DEFAULTS := {"rom_path":"", "save_path":"user://dq9.sav", "language":"en", "layout":0, "nearest":true, "integer_scale":false, "window_scale":1, "volume":0.8, "overlay_rect":Rect2(0.58, 0.56, 0.35, 0.35), "renderer_gpu3d":0, "gpu3d_scale":3, "issue_show_auto":false}

static func button_mask(pressed_actions: Array) -> int:
	var mask := 0
	for i in BUTTON_ACTIONS.size():
		if BUTTON_ACTIONS[i] in pressed_actions:
			mask |= 1 << BUTTON_BITS[i]
	return mask

static func layout_rects(view: Vector2, layout: int, overlay: Rect2 = Rect2(0.58, 0.56, 0.35, 0.35), integer_scale := false) -> Array[Rect2]:
	var result: Array[Rect2] = [Rect2(), Rect2()]
	var safe := Vector2(maxf(1, view.x), maxf(1, view.y))
	var top_size := Vector2(256, 192)
	var gap := 12.0
	var scale := minf(safe.x / 256.0, safe.y / 396.0)
	if layout == 1:
		scale = minf(safe.x / 524.0, safe.y / 192.0)
	if integer_scale:
		scale = maxf(1.0, floorf(scale))
	match layout:
		0:
			var size := Vector2(256, 396) * scale
			var origin := (safe - size) * 0.5
			result[0] = Rect2(origin, top_size * scale)
			result[1] = Rect2(origin + Vector2(0, 204 * scale), top_size * scale)
		1:
			var size := Vector2(524, 192) * scale
			var origin := (safe - size) * 0.5
			result[0] = Rect2(origin, top_size * scale)
			result[1] = Rect2(origin + Vector2(268 * scale, 0), top_size * scale)
		2, 3:
			var big := minf(safe.x / 256.0, (safe.y - gap) / (192.0 * 1.55))
			if integer_scale: big = maxf(1.0, floorf(big))
			var small := maxf(1.0, floorf(big * 0.55)) if integer_scale else big * 0.55
			var origin_y := (safe.y - 192.0 * (big + small) - gap) * 0.5
			var primary := Rect2(Vector2((safe.x - 256.0 * big) * 0.5, origin_y), top_size * big)
			var secondary_size := top_size * small
			var secondary := Rect2(Vector2((safe.x - secondary_size.x) * 0.5, origin_y + 192.0 * big + gap), secondary_size)
			if layout == 2: result[0] = primary; result[1] = secondary
			else: result[1] = primary; result[0] = secondary
		4, 5:
			var factor := minf(safe.x / 256.0, safe.y / 192.0)
			if integer_scale: factor = maxf(1.0, floorf(factor))
			result[0 if layout == 4 else 1] = Rect2((safe - top_size * factor) * 0.5, top_size * factor)
		6:
			var factor := minf(safe.x / 256.0, safe.y / 192.0)
			if integer_scale: factor = maxf(1.0, floorf(factor))
			result[0] = Rect2((safe - top_size * factor) * 0.5, top_size * factor)
			var r := overlay
			var bounds := Vector2(clampf(r.size.x, 0.15, 0.75) * safe.x, clampf(r.size.y, 0.15, 0.75) * safe.y)
			var overlay_scale := minf(bounds.x / 256.0, bounds.y / 192.0)
			if integer_scale: overlay_scale = maxf(1.0, floorf(overlay_scale))
			var size := top_size * overlay_scale
			var max_pos := Vector2.ONE - size / safe
			var pos := Vector2(clampf(r.position.x, 0, max_pos.x) * safe.x, clampf(r.position.y, 0, max_pos.y) * safe.y)
			result[1] = Rect2(pos, size)
	return result

# DS firmware language ids as used by the SDL frontend: en=1 fr=2 de=3 it=4 es=5 (not the launcher list order).
# First-run game language from the OS language (the European release ships en/fr/de/es/it).
static func language_for_locale(locale: String) -> String:
	var code := locale.to_lower().split("_")[0].split("-")[0]
	return code if code in ["en", "fr", "de", "es", "it"] else "en"

static func firmware_language(code: String) -> int:
	return {"en": 1, "fr": 2, "de": 3, "it": 4, "es": 5}.get(code.to_lower(), 1)

static func touch_coords(point: Vector2, rect: Rect2) -> Vector2i:
	if rect.size.x <= 0 or rect.size.y <= 0: return Vector2i.ZERO
	var p := (point - rect.position) / rect.size
	return Vector2i(clampi(roundi(clampf(p.x, 0, 1) * 255), 0, 255), clampi(roundi(clampf(p.y, 0, 1) * 191), 0, 191))

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
	values["layout"] = clampi(int(values["layout"]), 0, 6)
	values["renderer_gpu3d"] = clampi(int(values["renderer_gpu3d"]), 0, 1)
	values["gpu3d_scale"] = clampi(int(values["gpu3d_scale"]), 2, 6)
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
