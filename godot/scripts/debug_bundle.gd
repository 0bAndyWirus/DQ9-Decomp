extends RefCounted

static func save_note(machine: Node, directory: String, note: String, frame: int,
		rom_path: String, save_path: String, language: String) -> String:
	var absolute_dir := ProjectSettings.globalize_path(directory)
	if DirAccess.make_dir_recursive_absolute(absolute_dir) != OK:
		return "cannot create bundle directory: " + absolute_dir
	var stats: Dictionary = machine.get_stats()
	var top: Image = machine.get_top_image()
	var bottom: Image = machine.get_bottom_image()
	for pair in [[top, "screenshot_top.png"], [bottom, "screenshot_bottom.png"]]:
		var image: Image = pair[0]
		if image.save_png(directory.path_join(pair[1])) != OK:
			return "cannot write " + pair[1]
	var both := Image.create(256, 384, false, Image.FORMAT_RGBA8)
	both.blit_rect(top, Rect2i(0, 0, 256, 192), Vector2i(0, 0))
	both.blit_rect(bottom, Rect2i(0, 0, 256, 192), Vector2i(0, 192))
	if both.save_png(directory.path_join("screenshot_both.png")) != OK:
		return "cannot write combined screenshot"
	var lines: PackedStringArray = machine.get_input_history()
	var input_file := FileAccess.open(directory.path_join("session_input.txt"), FileAccess.WRITE)
	if input_file == null:
		return "cannot write session_input.txt"
	for line in lines: input_file.store_line(line)
	input_file.close()
	FileAccess.open(directory.path_join("session_input.txt.frames"), FileAccess.WRITE).store_line(str(lines.size()))
	var start_save: PackedByteArray = machine.get_session_start_save()
	var start_file := FileAccess.open(directory.path_join("session_start.sav"), FileAccess.WRITE)
	if start_file == null: return "cannot write session_start.sav"
	start_file.store_buffer(start_save)
	start_file.close()
	machine.flush_save()
	var current_save_path := ProjectSettings.globalize_path(save_path)
	var current_save := FileAccess.open(current_save_path, FileAccess.READ)
	if current_save == null: return "cannot read save at note: " + current_save_path
	var save_bytes := current_save.get_buffer(current_save.get_length())
	current_save.close()
	var note_save := FileAccess.open(directory.path_join("save_at_note.sav"), FileAccess.WRITE)
	if note_save == null: return "cannot write save_at_note.sav"
	note_save.store_buffer(save_bytes)
	note_save.close()
	var hash_context := HashingContext.new()
	if hash_context.start(HashingContext.HASH_SHA256) != OK: return "cannot initialize SHA-256"
	hash_context.update(save_bytes)
	var save_hash := hash_context.finish().hex_encode()
	var rtc_epoch := int(machine.get_rtc_epoch())
	var rtc_offset := int(machine.get_rtc_timezone_offset_minutes())
	var comment_file := FileAccess.open(directory.path_join("comment.txt"), FileAccess.WRITE)
	comment_file.store_string("%s\nframe: %d\nrtc_epoch: %d\nrtc_timezone_offset_minutes: %d\nsave_at_note: save_at_note.sav\nsave_at_note_sha256: %s\n" % [note, frame, rtc_epoch, rtc_offset, save_hash])
	comment_file.close()
	var state_file := FileAccess.open(directory.path_join("state.txt"), FileAccess.WRITE)
	state_file.store_string("frame=%d\nfps=%s\naudio_queue_ms=%s\nunderruns=%s\n" % [frame, stats.get("fps", 0), stats.get("audio_queue_ms", 0), stats.get("underruns", 0)])
	state_file.close()
	var replay_binary := ProjectSettings.globalize_path("res://../build/game-fast/game/dq9")
	var absolute_rom := ProjectSettings.globalize_path(rom_path)
	# The save chip decides the save file layout, so the replay must use the same one.
	var chip: String = machine.get_backup_type() if machine and machine.has_method("get_backup_type") else ""
	var chip_argument := "" if chip.is_empty() or chip == "auto" else " --backup-type " + _shell_quote(chip)
	var replay := FileAccess.open(directory.path_join("replay.sh"), FileAccess.WRITE)
	if replay == null: return "cannot write replay.sh"
	replay.store_string("#!/bin/sh\nset -eu\nSCRIPT_DIR=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\nOUT=\"$SCRIPT_DIR/replay_out\"\nmkdir -p \"$OUT\"\ncp \"$SCRIPT_DIR/session_start.sav\" \"$OUT/replay.sav\"\nexec %s --rom %s --save 'replay_out/replay.sav' --language %s%s --rtc-epoch %d --rtc-timezone-offset %d --headless --frames %d --input-script 'session_input.txt' --dump-frames %d --dump-dir 'replay_out' --dump-audio 'replay_out/audio.wav'\n" % [
		_shell_quote(replay_binary), _shell_quote(absolute_rom), _shell_quote(language), chip_argument, rtc_epoch, rtc_offset, frame + 1, frame])
	replay.close()
	var env := FileAccess.open(directory.path_join("env.txt"), FileAccess.WRITE)
	env.store_string("rom_path: %s\nsave_path: %s\nlanguage: %s\nrtc_epoch: %d\nrtc_timezone_offset_minutes: %d\n" % [absolute_rom, current_save_path, language, rtc_epoch, rtc_offset])
	env.close()
	return ""

static func _shell_quote(value: String) -> String:
	return "'" + value.replace("'", "'\\''") + "'"
