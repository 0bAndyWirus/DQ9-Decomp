extends RefCounted

# The automatic rules run on the emulation owner. Only the manual presentation
# override lives here; generation changes include keys skipped by the mailbox.
var _main_key_generation := -1
var _main_override := -1

func reset() -> void:
	_main_key_generation = -1
	_main_override = -1

func presented_main_screen(snapshot: Dictionary) -> int:
	var generation := int(snapshot.get("main_key_generation", -1))
	if generation != _main_key_generation:
		_main_key_generation = generation
		_main_override = -1
	return _main_override if _main_override >= 0 else int(snapshot.get("main_screen", -1))

func swap_main_screen(current_bottom: bool) -> void:
	_main_override = 0 if current_bottom else 1

static func widescreen_crop(mode: int, scene: String) -> bool:
	return mode in [1, 2] and scene in ["intro", "character_creation"]
