extends SceneTree
const Store = preload("res://scripts/issue_store.gd")
const Fixture = preload("res://tests/issue_fixture.gd")
var _ok := true

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	if OS.has_feature("linuxbsd") or OS.has_feature("windows") or OS.has_feature("macos"):
		var engine_dir := "godot" if OS.has_feature("linuxbsd") else "Godot"
		var legacy_user := OS.get_data_dir().path_join(engine_dir).path_join("app_userdata/DQ9-Decomp")
		_expect(ProjectSettings.globalize_path("user://").simplify_path().trim_suffix("/") == legacy_user.simplify_path(), "renamed project retains legacy user:// directory")
	var base := ProjectSettings.globalize_path("user://issue-store-test-%d" % Time.get_ticks_usec())
	var roots := Fixture.make_synthetic(base)
	var originals := Fixture.snapshot_originals(roots)
	var store := Store.new(roots)
	var issues := store.list_issues()
	_expect(issues.size() == 2, "auto notes hidden by default")
	if issues.size() < 2: quit(1); return
	_expect(issues[0].date == "2026-10-12T12:00:00" and issues[1].date == "2026-10-11T12:00:00", "newest first across SDL / Godot timestamp formats")
	_expect(store.list_issues("all", "", true).size() == 3, "auto-note toggle")
	_expect(store.list_issues("open").size() == 1 and store.list_issues("solved").size() == 1, "status filters")
	_expect(store.list_issues("all", "CREAKING").size() == 1 and store.list_issues("all", "missing").is_empty(), "case-insensitive comment search")
	_expect(not issues[0].audio_warning.is_empty(), "solo/mute warning is visible evidence")
	_expect(Store.audio_warning("audio_debug: solo=-1 muted=none").is_empty(), "normal audio is not warned")
	_expect(not Store.audio_warning("audio_solo=0\naudio_muted=none").is_empty(), "solo channel zero warning")
	_expect(Store.audio_warning("audio_mute=false").is_empty(), "false mute is not warned")
	var path: String = issues[0].path
	for status in ["testing", "solved", "open", "wontfix"]:
		_expect(store.set_status(path, status, "note for " + status).is_empty(), "write status " + status)
		_expect(Store.new(roots).read_status(path).status == status, "persist status " + status)
	var data := store.read_status(path)
	_expect(data.history.size() == 4 and data.history[3].note == "note for wontfix", "history appended with notes")
	_expect(data.updated.ends_with("Z") and data.history[0].time.contains("T"), "ISO UTC timestamps")
	_expect(store.add_retest_note(path, "Retest observation").is_empty(), "persist retest note")
	_expect(store.read_status(path).history.size() == 5, "retest appends history without changing status")
	_expect(store.record_test(path, 299, 17, "test-build").is_empty(), "record informational pixel result")
	data = store.read_status(path)
	_expect(data.status == "wontfix" and data.last_test.frame == 299 and data.last_test.result_pixels_differing == 17 and data.last_test.build == "test-build", "last_test schema, no automatic pass/fail transition")
	_expect(not store.set_status(path, "invalid").is_empty() and not store.add_retest_note(path, " ").is_empty(), "reject invalid status/empty note")
	_expect(store.export_report(base.path_join("report.txt")).is_empty(), "export all issues")
	var report := FileAccess.get_file_as_string(base.path_join("report.txt"))
	_expect(report.contains("[wontfix]") and report.contains("[solved]") and report.contains("[auto]") and report.contains("Retest observation"), "report includes statuses, history and hidden auto notes")
	_expect(not store.export_report(path.path_join("comment.txt")).is_empty(), "export cannot overwrite original evidence")
	_expect(originals == Fixture.snapshot_originals(roots), "only status.json changed inside bundles")
	var current := base.path_join("current/shared")
	var legacy := base.path_join("legacy/shared")
	DirAccess.make_dir_recursive_absolute(current)
	DirAccess.make_dir_recursive_absolute(legacy)
	var current_file := FileAccess.open(current.path_join("comment.txt"), FileAccess.WRITE)
	current_file.store_string("current evidence")
	current_file.close()
	var legacy_file := FileAccess.open(legacy.path_join("comment.txt"), FileAccess.WRITE)
	legacy_file.store_string("legacy evidence")
	legacy_file.close()
	var fallback := Store.new(PackedStringArray([base.path_join("current"), base.path_join("legacy")]))
	var duplicates := fallback.list_issues()
	_expect(duplicates.size() == 1 and duplicates[0].path == current, "duplicate bundle names prefer current root")
	_expect(FileAccess.get_file_as_string(legacy.path_join("comment.txt")) == "legacy evidence", "legacy evidence remains untouched")
	DirAccess.remove_absolute(current.path_join("comment.txt"))
	var legacy_only := fallback.list_issues()
	_expect(legacy_only.size() == 1 and legacy_only[0].path == legacy, "legacy bundle remains visible when current is absent")
	Fixture.remove_tree(base)
	print("PASS: issue store listing/sorting/filtering, transitions/history, last_test, export, immutable evidence" if _ok else "FAIL: issue store")
	quit(0 if _ok else 1)

func _expect(condition: bool, message: String) -> void:
	if not condition: _ok = false; push_error(message)
