extends SceneTree
# ROM-free coverage of engine-class bindings, child-first deletion and shutdown
# while native presentation nodes are still in the scene tree.
var failures: Array[String] = []

func check(condition: bool, message: String) -> void:
	if not condition:
		failures.append(message)
		push_error(message)

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	check(ClassDB.class_exists("DqMachine"), "DqMachine is not registered")
	check(ClassDB.class_exists("DqGpuScene"), "DqGpuScene is not registered")
	if not failures.is_empty():
		quit(1)
		return
	for iteration in 4:
		var parent := Node.new()
		root.add_child(parent)
		var machine = ClassDB.instantiate("DqMachine")
		parent.add_child(machine)
		await process_frame
		check(machine.get_top_image() is Image, "Image instance binding is missing")
		check(machine.get_bottom_image() is Image, "bottom Image binding is missing")
		check(machine.get_top_texture() is ImageTexture, "ImageTexture binding is missing")
		check(machine.get_audio_player().get_stream_playback() is AudioStreamGeneratorPlayback,
			"audio playback instance binding is missing")
		# The server may retain a stopped playback for its final fade-out mix.
		# That playback must own the generator after DqMachine has released it.
		var playback: AudioStreamGeneratorPlayback = machine.get_audio_player().get_stream_playback()
		if playback != null:
			check(playback.get_meta("_dq9_generator", null) is AudioStreamGenerator,
				"audio playback does not retain its generator during asynchronous stop")
		playback = null
		var image_watch: WeakRef = weakref(machine.get_top_image())
		var texture_watch: WeakRef = weakref(machine.get_top_texture())
		if ClassDB.class_exists("DqGpuSceneFixture"):
			var fixture = ClassDB.instantiate("DqGpuSceneFixture")
			parent.add_child(fixture)
			for frame in 3:
				fixture.advance()
			check(fixture.get_scene() is Node, "native GPU scene was not created")
		parent.free()
		check(image_watch.get_ref() == null, "machine destruction retains its Image")
		check(texture_watch.get_ref() == null, "machine destruction retains its ImageTexture")
	# Do not free these nodes ourselves: the main-loop shutdown callback must
	# release their references before Godot's later Node/extension teardown.
	var shutdown_machine = ClassDB.instantiate("DqMachine")
	root.add_child(shutdown_machine)
	if ClassDB.class_exists("DqGpuSceneFixture"):
		var shutdown_fixture = ClassDB.instantiate("DqGpuSceneFixture")
		root.add_child(shutdown_fixture)
		for frame in 3:
			shutdown_fixture.advance()
	await process_frame
	print("PASS: DqMachine registered; extension lifecycle" if failures.is_empty() else "FAIL: extension lifecycle")
	quit(0 if failures.is_empty() else 1)
