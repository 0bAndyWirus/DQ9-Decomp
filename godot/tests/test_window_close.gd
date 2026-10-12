extends SceneTree
# Run under X11 with --rom/--save/--autostart. Keep a living X client with
# WM_DELETE_WINDOW/WM_PROTOCOLS interned before launch, then send WM_DELETE_WINDOW
# after WINDOW_CLOSE_READY. A missing close event fails after 15 seconds.
func _initialize() -> void: call_deferred("_run")
func click(app: Control, text: String) -> void:
	var target: Button=null
	for node in app.find_children("*","Button",true,false):
		if node.text==text: target=node
	assert(target!=null)
	var pos:=target.get_global_rect().get_center()
	OS.execute("xdotool",["mousemove","--window",str(DisplayServer.window_get_native_handle(DisplayServer.WINDOW_HANDLE)),str(roundi(pos.x)),str(roundi(pos.y)),"click","1"])
	await create_timer(0.3).timeout
func _run() -> void:
	var app=load("res://scenes/main.tscn").instantiate(); root.add_child(app)
	if app.machine==null: push_error("Native autostart required"); quit(2); return
	await create_timer(2).timeout
	app._toggle_pause()
	await process_frame
	await click(app,"Close game")
	if app.machine!=null or not app.settings_panel.visible:
		push_error("Routed Close game did not release machine and show menu"); quit(1); return
	if not FileAccess.file_exists(app.settings.save_path):
		push_error("Close game did not flush save"); quit(1); return
	await click(app,"Start")
	if app.machine==null: push_error("Restart from menu failed"); quit(1); return
	await create_timer(2).timeout
	print("WINDOW_CLOSE_READY ",DisplayServer.window_get_native_handle(DisplayServer.WINDOW_HANDLE))
	await create_timer(15).timeout
	push_error("No window-close event received before deadline")
	app._release_machine(false); app.free(); quit(1)
