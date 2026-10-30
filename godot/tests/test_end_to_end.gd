extends SceneTree
# Graphical native QA. Requires xdotool, --rom/--save, --qa-input,
# --qa-frames and --qa-checkpoints; --qa-out must be local.
const Logic = preload("res://scripts/host_logic.gd")
var app
var out := ""
var failures: Array[String] = []
func _initialize() -> void: call_deferred("_run")
func check(value: bool, message: String) -> void:
	if not value: failures.append(message); push_error(message)
func capture(name: String) -> void:
	await process_frame
	await RenderingServer.frame_post_draw
	check(root.get_texture().get_image().save_png(out.path_join(name+".png")) == OK,"capture "+name)
func button(text: String) -> Button:
	var result: Button = null
	for node in app.find_children("*","Button",true,false):
		if node.text == text: result=node
	return result
func click(control: Control) -> void:
	var pos := control.get_global_rect().get_center()
	OS.execute("xdotool",["mousemove","--window",str(DisplayServer.window_get_native_handle(DisplayServer.WINDOW_HANDLE)),str(roundi(pos.x)),str(roundi(pos.y)),"click","1"])
	await create_timer(0.2).timeout
func key(name: String, down: bool) -> void:
	OS.execute("xdotool",["keydown" if down else "keyup",name])
	await create_timer(0.25).timeout
func _run() -> void:
	var args := OS.get_cmdline_user_args()
	var input_path := ""
	var replay_frames := 0
	var replay_checkpoints := PackedInt32Array()
	for i in args.size():
		if args[i] == "--qa-out" and i+1<args.size(): out=args[i+1]
		if args[i] == "--qa-input" and i+1<args.size(): input_path=args[i+1]
		if args[i] == "--qa-frames" and i+1<args.size(): replay_frames=int(args[i+1])
		if args[i] == "--qa-checkpoints" and i+1<args.size():
			for value in args[i+1].split(","): replay_checkpoints.append(int(value))
	if out.is_empty() or input_path.is_empty() or replay_frames < 1 or replay_checkpoints.is_empty():
		push_error("--qa-out, --qa-input, --qa-frames and --qa-checkpoints required"); quit(2); return
	for checkpoint in replay_checkpoints:
		if checkpoint < 0 or checkpoint >= replay_frames:
			push_error("capture checkpoint is outside replay"); quit(2); return
	DirAccess.make_dir_recursive_absolute(out)
	app=load("res://scenes/main.tscn").instantiate(); root.add_child(app)
	await process_frame
	await process_frame
	check(app.launcher.visible,"Launcher appears")
	await capture("menu")
	app.launcher.show_wizard()
	await process_frame
	await click(app.launcher._browse)
	await process_frame
	var dialog: FileDialog=app.launcher._dialog
	check(dialog.visible and dialog.file_mode==FileDialog.FILE_MODE_OPEN_FILE,"ROM FileDialog")
	await capture("rom_picker")
	dialog.file_selected.emit(app.settings.rom_path)
	dialog.hide()
	await process_frame
	check(not app.settings.rom_path.is_empty(),"ROM selection accepted")
	app._show_launcher()
	await click(app.launcher._continue)
	check(app.machine != null,"Start click creates native machine")
	if app.machine==null: quit(1); return
	await create_timer(2).timeout
	await capture("boot")
	Logic.reset_bindings()
	var keys := ["z","x","a","s","Return","BackSpace","Up","Down","Left","Right","e","q"]
	for i in keys.size():
		print("QA_KEY ",keys[i])
		await key(keys[i],true)
		var observed := false
		for row in app.machine.get_input_history():
			if int(str(row).split(" ")[1]) & (1 << Logic.BUTTON_BITS[i]): observed=true
		check(observed,"xdotool DS mapping "+Logic.BUTTON_ACTIONS[i])
		print("QA_KEY_OBSERVED ",keys[i]," ",observed)
		await key(keys[i],false)
	app._set_layout(0)
	await process_frame
	for fraction in [Vector2(0.1,0.1),Vector2(0.5,0.5),Vector2(0.9,0.9)]:
		var rect: Rect2=app.bottom_rect.get_global_rect()
		var point: Vector2=rect.position+fraction*rect.size
		OS.execute("xdotool",["mousemove","--window",str(DisplayServer.window_get_native_handle(DisplayServer.WINDOW_HANDLE)),str(roundi(point.x)),str(roundi(point.y)),"mousedown","1"])
		await create_timer(0.3).timeout
		var expected := Logic.touch_coords(point,rect)
		var observed := false
		for row in app.machine.get_input_history():
			var fields:=str(row).split(" ")
			if absi(int(fields[2])-expected.x)<=1 and absi(int(fields[3])-expected.y)<=1: observed=true
		check(observed,"touch DS coordinate "+str(expected))
		OS.execute("xdotool",["mouseup","1"])
		await process_frame
	# Exercise binding through actual Input.parse_input_event dispatch.
	app._show_settings()
	var pages = app._ui.settings_pages
	pages._begin_binding("ds_a")
	var rebound:=InputEventKey.new(); rebound.physical_keycode=KEY_C; rebound.keycode=KEY_C; rebound.pressed=true
	Input.parse_input_event(rebound); await process_frame
	check(pages._binding_action.is_empty() and InputMap.action_has_event("ds_a",rebound),"Keyboard rebind")
	rebound.pressed=false; Input.parse_input_event(rebound)
	pages._begin_binding("ds_b")
	var joy:=InputEventJoypadButton.new(); joy.button_index=JOY_BUTTON_A; joy.pressed=true
	Input.parse_input_event(joy); await process_frame
	check(pages._binding_action.is_empty() and InputMap.action_has_event("ds_b",joy),"Joypad rebind")
	var cfg := ConfigFile.new(); cfg.load("user://settings.cfg")
	check(cfg.get_value("bindings","ds_b",[])[0].kind=="button","Joypad binding persisted")
	joy.pressed=false; Input.parse_input_event(joy)
	Logic.reset_bindings()
	app._hide_settings()
	app._toggle_pause()
	for i in Logic.BUTTON_ACTIONS.size():
		var pad := InputMap.action_get_events(Logic.BUTTON_ACTIONS[i])[1] as InputEventJoypadButton
		pad.pressed=true; Input.parse_input_event(pad)
		await create_timer(0.2).timeout
		check(Input.is_action_pressed(Logic.BUTTON_ACTIONS[i]),"Injected joypad action "+Logic.BUTTON_ACTIONS[i])
		pad.pressed=false; Input.parse_input_event(pad)
		await process_frame
	# Replay the supplied local input under both language selections.
	for lang in ["en","fr"]:
		print("QA_RESTART ",lang)
		app._stop_game()
		app._show_settings()
		pages._controls.language.item_selected.emit(app.LANGS.find(lang))
		app._hide_settings()
		await click(app.launcher._continue)
		app.machine.stop()
		# Reload stopped for deterministic frame zero: Start's realtime worker may
		# already have completed one frame before the test can stop it.
		check(app.machine.load_game(app.settings.rom_path,app.settings.save_path,Logic.firmware_language(lang)).is_empty(),"Deterministic language replay load")
		check(app.machine.get_session_start_save()==FileAccess.get_file_as_bytes(app.settings.save_path),"Restart loads flushed save")
		check(app.machine.get_ram_snapshot().is_empty(),"No snapshot before registration")
		check(not app.machine.register_snapshot_range(0x01000000,1).is_empty(),"Reject non-allowlisted memory")
		check(not app.machine.register_snapshot_range(0x023fffff,2).is_empty(),"Reject crossing RAM boundary")
		check(app.machine.register_snapshot_range(0x02000000,16).is_empty(),"Register RAM snapshot")
		check(app.machine.get_ram_snapshot().is_empty(),"Snapshot waits for consumed frame")
		var inputs := {}
		var file:=FileAccess.open(input_path,FileAccess.READ)
		while not file.eof_reached():
			var fields:=file.get_line().split(" ",false)
			if fields.size()==4: inputs[int(fields[0])]=int(fields[1])
		file.close()
		for frame in replay_frames:
			app.machine.step_frames(1,inputs.get(frame,0),0,0,false)
			await app.machine.frame_ready
			if frame == 0:
				var snapshot: Dictionary = app.machine.get_ram_snapshot()
				check(snapshot.get("frame",-1)==1,"Snapshot frame matches consumed frame")
				check(snapshot.get("ranges",[]).size()==1,"Snapshot contains registered range")
				if snapshot.get("ranges",[]).size()==1:
					check(snapshot.ranges[0].address==0x02000000 and snapshot.ranges[0].bytes.size()==16,"Snapshot address and size")
					snapshot.ranges[0].address=0
					check(app.machine.get_ram_snapshot().ranges[0].address==0x02000000,"Snapshot result is independent")
			if frame in replay_checkpoints: await capture("replay_%s_%d"%[lang,frame])
		await capture("replay_"+lang)
		app.machine.clear_snapshot_ranges()
		check(app.machine.register_snapshot_range(0x01ffd348,4).is_empty(),"Register relocated ITCM range")
		app.machine.clear_snapshot_ranges()
		check(app.machine.get_ram_snapshot().is_empty(),"Clearing registrations invalidates snapshot")
	check(FileAccess.file_exists(app.settings.save_path),"Restart flushed save")
	root.size = Vector2i(3072,1920)
	await create_timer(0.2).timeout
	check(root.get_texture().get_width()==3072 and root.get_texture().get_height()==1920,"Physical viewport follows 3072x1920 window")
	for layout in 7:
		app._set_layout(layout)
		await capture("layout_%d"%layout)
		for texture in [app.top_rect,app.bottom_rect]:
			if texture.is_visible_in_tree(): check(absf(texture.size.x/texture.size.y-4.0/3.0)<0.001,"Native texture aspect")
	app.settings.integer_scale=true; app._layout_screens(); await capture("integer")
	app.settings.nearest=false; app._apply_filter(); await capture("filtered")
	check(app.top_rect.texture_filter==CanvasItem.TEXTURE_FILTER_LINEAR,"Filter changed")
	app.settings.nearest=true; app.settings.integer_scale=false; app._apply_filter()
	app._set_layout(6); await process_frame
	var start: Vector2=app.bottom_rect.get_global_rect().get_center()
	var old: Rect2=app._overlay_rect
	var win:=str(DisplayServer.window_get_native_handle(DisplayServer.WINDOW_HANDLE))
	OS.execute("xdotool",["keydown","Alt_L","mousemove","--window",win,str(roundi(start.x)),str(roundi(start.y)),"mousedown","1","mousemove_relative","--","-100","-100"])
	await create_timer(0.3).timeout
	OS.execute("xdotool",["mouseup","1","keyup","Alt_L"])
	check(app._overlay_rect.position!=old.position,"Overlay Alt-drag")
	await capture("overlay_drag")
	start=app.bottom_rect.get_global_rect().get_center()
	OS.execute("xdotool",["keydown","Alt_L","mousemove","--window",win,str(roundi(start.x)),str(roundi(start.y)),"mousedown","3","mousemove_relative","--","100","100"])
	await create_timer(0.3).timeout
	OS.execute("xdotool",["mouseup","3","keyup","Alt_L"])
	check(app._overlay_rect.size!=old.size,"Overlay Alt-resize")
	await capture("overlay_resize")
	await key("F11",true); await key("F11",false)
	check(DisplayServer.window_get_mode()==DisplayServer.WINDOW_MODE_FULLSCREEN,"F11 fullscreen")
	await capture("fullscreen")
	await key("F11",true); await key("F11",false)
	app._toggle_pause()
	if not app.pause_panel.visible: app._toggle_pause()
	await create_timer(0.2).timeout
	var frozen:int=app.machine.get_stats().frame
	await create_timer(0.3).timeout
	check(app.machine.get_stats().frame==frozen,"Pause freezes frame")
	app._ui.pause_tabs.current_tab = 1
	await process_frame
	await click(button("Step one frame"))
	check(app.machine.get_stats().frame==frozen+1,"Step advances one")
	await capture("audio_stats")
	var speed: OptionButton=app.pause_panel.find_children("*","OptionButton",true,false)[0]
	for index in 5: speed.item_selected.emit(index)
	speed.item_selected.emit(2)
	var stats_toggle: CheckButton=app.pause_panel.find_children("*","CheckButton",true,false)[0]
	stats_toggle.toggled.emit(false); check(not app.status_label.visible,"Stats hide")
	stats_toggle.toggled.emit(true); check(app.status_label.visible,"Stats show")
	app.pause_panel.hide(); app.machine.start()
	var t0:=Time.get_ticks_msec(); var f0:int=app.machine.get_stats().frame
	await create_timer(10).timeout
	print("PERF_3072 fps=",float(app.machine.get_stats().frame-f0)*1000.0/(Time.get_ticks_msec()-t0)," stats=",app.machine.get_stats())
	root.content_scale_factor=1.5
	await capture("hidpi_150")
	check(absf(app.top_rect.size.x/app.top_rect.size.y-4.0/3.0)<0.001,"HiDPI aspect")
	root.content_scale_factor=1.0
	app._persist()
	var stored:=Logic.load_settings("user://settings.cfg")
	check(stored.language=="fr" and stored.layout==6 and stored.overlay_rect==app._overlay_rect,"Settings persist")
	app._stop_game(); check(app.machine==null and app.launcher.visible,"Close game")
	app.settings.rom_path="/nonexistent/qa.nds"; await app._start_game()
	check(app.machine==null and app.launcher.visible and not app.launcher._status.text.is_empty(),"ROM error shown in launcher")
	await capture("invalid_rom")
	app.free()
	print("E2E failures=",failures)
	quit(0 if failures.is_empty() else 1)
