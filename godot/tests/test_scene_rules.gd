extends SceneTree

const Rules = preload("res://scripts/scene_rules.gd")

func _initialize() -> void:
	var ok := true
	var rules = Rules.new()
	var snapshot := {"main_key_generation": 1, "main_screen": -1}
	if rules.presented_main_screen(snapshot) != -1: ok = false
	rules.swap_main_screen(false)
	if rules.presented_main_screen(snapshot) != 1: ok = false
	snapshot.main_screen = 1
	if rules.presented_main_screen(snapshot) != 1: ok = false
	# A change and return between presented frames still invalidates F8.
	snapshot.main_screen = -1
	snapshot.main_key_generation = 3
	if rules.presented_main_screen(snapshot) != -1: ok = false
	snapshot.main_screen = 1
	rules.swap_main_screen(true)
	if rules.presented_main_screen(snapshot) != 0: ok = false
	rules.reset()
	if rules.presented_main_screen(snapshot) != 1: ok = false
	for mode in 3:
		for scene in ["other", "battle", "character_creation", "intro", "title", "field", "town"]:
			var expected: bool = mode in [1, 2] and scene in ["intro", "character_creation"]
			if Rules.widescreen_crop(mode, scene) != expected:
				push_error("unexpected crop: %d %s" % [mode, scene])
				ok = false
	if not ok: push_error("scene presentation override regression")
	quit(0 if ok else 1)
