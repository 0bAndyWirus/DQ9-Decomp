extends RefCounted
# Bundles are immutable evidence. Only status.json is owned by the reviewer.

const STATUSES := ["open", "testing", "solved", "wontfix"]
var roots: PackedStringArray

func _init(bundle_roots: PackedStringArray = PackedStringArray()) -> void:
	roots = bundle_roots
	if roots.is_empty():
		var data_home := OS.get_environment("XDG_DATA_HOME")
		if data_home.is_empty(): data_home = OS.get_environment("HOME").path_join(".local/share")
		roots = PackedStringArray([data_home.path_join("dq9-remastered/debug"),
			data_home.path_join("dq9-decomp/debug"), ProjectSettings.globalize_path("user://debug")])

func list_issues(filter := "all", search := "", show_auto := false) -> Array[Dictionary]:
	var issues: Array[Dictionary] = []
	var seen := {}
	for root in roots:
		var absolute := ProjectSettings.globalize_path(root)
		var directory := DirAccess.open(absolute)
		if directory == null: continue
		for name in directory.get_directories():
			var path := absolute.path_join(name).simplify_path()
			if seen.has(name) or not FileAccess.file_exists(path.path_join("comment.txt")): continue
			seen[name] = true
			var issue := read_issue(path)
			var status: String = issue.status.status
			if filter == "open" and status not in ["open", "testing"]: continue
			if filter == "solved" and status != "solved": continue
			if not show_auto and issue.auto: continue
			if not search.is_empty() and not issue.comment.to_lower().contains(search.to_lower()): continue
			issues.append(issue)
	issues.sort_custom(func(a: Dictionary, b: Dictionary):
		if a.sort_time == b.sort_time: return a.path > b.path
		return a.sort_time > b.sort_time)
	return issues

func read_issue(path: String) -> Dictionary:
	var comment := _text(path.path_join("comment.txt"))
	var state := _text(path.path_join("state.txt"))
	var metadata := fields(comment)
	var environment := fields(_text(path.path_join("env.txt")))
	var state_fields := fields(state)
	var date := _date_from_name(path.get_file())
	var sort_time := int(FileAccess.get_modified_time(path.path_join("comment.txt")))
	if not date.is_empty(): sort_time = int(Time.get_unix_time_from_datetime_string(date))
	else: date = Time.get_datetime_string_from_unix_time(sort_time)
	var note_lines := PackedStringArray()
	for line in comment.split("\n"):
		var key := line.get_slice(":", 0).strip_edges()
		if key in ["audio_debug", "timestamp", "local_time", "frame", "emulated_ticks", "wall_seconds_since_launch", "rtc_epoch", "rtc_timezone_offset_minutes", "save_at_note", "save_at_note_sha256"]: continue
		note_lines.append(line)
	var note := "\n".join(note_lines).strip_edges()
	return {"path": path, "comment": comment, "note": note, "state": state,
		"audio": _text(path.path_join("audio_stats.txt")), "env": environment,
		"frame": int(metadata.get("frame", state_fields.get("frame", -1))),
		"language": str(environment.get("language", "en")), "date": date,
		"sort_time": sort_time, "auto": note.to_lower().begins_with("[auto]"),
		"thumbnail": path.path_join("screenshot_both.png"), "status": read_status(path),
		"audio_warning": audio_warning(state)}

func read_status(path: String) -> Dictionary:
	var text := _text(path.path_join("status.json"))
	var data = JSON.parse_string(text) if not text.is_empty() else {}
	if not data is Dictionary: data = {}
	var status: String = str(data.get("status", "open"))
	if status not in STATUSES: status = "open"
	return {"status": status, "updated": str(data.get("updated", "")),
		"history": data.get("history", []) if data.get("history", []) is Array else [],
		"last_test": data.get("last_test", {}) if data.get("last_test", {}) is Dictionary else {}}

func set_status(path: String, status: String, note := "") -> String:
	if status not in STATUSES: return "Unknown issue status: " + status
	var data := read_status(path)
	data.status = status
	_append_history(data, note)
	return _save(path, data)

func add_retest_note(path: String, note: String) -> String:
	if note.strip_edges().is_empty(): return "Enter a retest note first."
	var data := read_status(path)
	_append_history(data, note)
	return _save(path, data)

func record_test(path: String, frame: int, pixels, build: String) -> String:
	var data := read_status(path)
	data.last_test = {"time": _now(), "frame": frame, "result_pixels_differing": pixels, "build": build}
	_append_history(data, "Replayed to frame %d" % frame if pixels == null else "Compared frame %d: %d differing pixels (informational)" % [frame, int(pixels)])
	return _save(path, data)

func export_report(path: String) -> String:
	var destination := ProjectSettings.globalize_path(path).simplify_path()
	for root in roots:
		var bundle_root := ProjectSettings.globalize_path(root).simplify_path().trim_suffix("/")
		if destination == bundle_root or destination.begins_with(bundle_root + "/"):
			return "Export reports outside the bundle folders so original evidence stays untouched."
	var lines := PackedStringArray(["DQ9-Remastered issue review — " + _now(), "Pixel differences are informational, not a pass/fail result.", ""])
	for issue in list_issues("all", "", true):
		lines.append("[%s] %s | frame %d | %s | %s" % [issue.status.status, issue.date, issue.frame, issue.language, issue.path])
		lines.append(issue.note)
		for entry in issue.status.history:
			if entry is Dictionary: lines.append("  %s [%s] %s" % [entry.get("time", ""), entry.get("status", ""), entry.get("note", "")])
		if not issue.status.last_test.is_empty(): lines.append("  Last test: " + JSON.stringify(issue.status.last_test))
		lines.append("")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null: return "Cannot write report: " + path
	file.store_string("\n".join(lines))
	var error := file.get_error()
	file.close()
	return "" if error == OK else "Cannot finish writing report: " + path

static func fields(text: String) -> Dictionary:
	var result := {}
	for line in text.split("\n"):
		var colon := line.find(":")
		var equals := line.find("=")
		var split := colon if colon >= 0 and (equals < 0 or colon < equals) else equals
		if split < 0: continue
		result[line.left(split).strip_edges()] = line.substr(split + 1).strip_edges()
	return result

static func audio_warning(state: String) -> String:
	var expression := RegEx.new()
	expression.compile("(?:audio_debug:[^\\n]*?solo=|audio_solo[:=]\\s*)(-?\\d+)")
	var solo := expression.search(state)
	expression.compile("(?:audio_debug:[^\\n]*?muted=|audio_muted[:=]\\s*|audio_mute[:=]\\s*)([^\\n]+)")
	var muted := expression.search(state)
	var solo_active := solo != null and int(solo.get_string(1)) >= 0
	var mute_active := muted != null and muted.get_string(1).strip_edges().to_lower() not in ["none", "false", "0", "-1", ""]
	if solo_active or mute_active:
		return "WARNING: original capture used audio solo/mute. Replay uses normal audio; audio symptoms may differ."
	return ""

static func _text(path: String) -> String:
	return FileAccess.get_file_as_string(path) if FileAccess.file_exists(path) else ""

static func _date_from_name(name: String) -> String:
	var expression := RegEx.new()
	expression.compile("^(\\d{4})(\\d{2})(\\d{2})-(\\d{2})(\\d{2})(\\d{2})")
	var match_result := expression.search(name)
	if match_result != null:
		return "%s-%s-%sT%s:%s:%s" % [match_result.get_string(1), match_result.get_string(2), match_result.get_string(3), match_result.get_string(4), match_result.get_string(5), match_result.get_string(6)]
	expression.compile("^(\\d{4}-\\d{2}-\\d{2})[T_ ](\\d{2})[-:](\\d{2})[-:](\\d{2})")
	match_result = expression.search(name)
	return "%sT%s:%s:%s" % [match_result.get_string(1), match_result.get_string(2), match_result.get_string(3), match_result.get_string(4)] if match_result != null else ""

static func _now() -> String:
	return Time.get_datetime_string_from_system(true) + "Z"

static func _append_history(data: Dictionary, note: String) -> void:
	data.updated = _now()
	data.history.append({"time": data.updated, "status": data.status, "note": note.strip_edges()})

static func _save(path: String, data: Dictionary) -> String:
	if not FileAccess.file_exists(path.path_join("comment.txt")): return "Bundle no longer exists: " + path
	var file := FileAccess.open(path.path_join("status.json"), FileAccess.WRITE)
	if file == null: return "Cannot write status.json in " + path
	file.store_string(JSON.stringify(data, "\t") + "\n")
	var error := file.get_error()
	file.close()
	return "" if error == OK else "Cannot finish writing status.json in " + path
