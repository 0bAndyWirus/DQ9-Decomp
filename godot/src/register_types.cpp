#include "dq_gpu_scene.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

void register_dq_machine_class();
void shutdown_dq_machine_instances();
#ifdef DQ9_GPU_SCENE_TESTS
void register_gpu_scene_fixture();
void shutdown_gpu_scene_fixture_instances();
#endif

namespace {
void initialize_module(godot::ModuleInitializationLevel level) {
    if (level != godot::MODULE_INITIALIZATION_LEVEL_SCENE) return;
    register_dq_machine_class();
    godot::ClassDB::register_class<DqGpuScene>();
#ifdef DQ9_GPU_SCENE_TESTS
    register_gpu_scene_fixture();
#endif
}
void shutdown_module() {
    shutdown_dq_machine_instances();
#ifdef DQ9_GPU_SCENE_TESTS
    shutdown_gpu_scene_fixture_instances();
#endif
    DqGpuScene::shutdown_instances();
}
void uninitialize_module(godot::ModuleInitializationLevel level) {
    if (level != godot::MODULE_INITIALIZATION_LEVEL_SCENE) return;
    // Run before godot-cpp unregisters classes/frees instance bindings, never
    // from DLL static destructors under Windows' process-exit loader lock.
    shutdown_module();
}
}

extern "C" GDExtensionBool GDE_EXPORT dq9_godot_init(GDExtensionInterfaceGetProcAddress get_proc_address,
        GDExtensionClassLibraryPtr library, GDExtensionInitialization* initialization) {
    godot::GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize_module);
    init.register_terminator(uninitialize_module);
    // The engine is still fully initialized at this hook. The scene-level
    // terminator is the idempotent fallback for an explicit extension unload.
    init.register_shutdown_callback(shutdown_module);
    init.set_minimum_library_initialization_level(godot::MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
