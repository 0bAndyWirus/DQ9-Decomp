#include "dq_gpu_scene.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/hashing_context.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
using namespace godot;
constexpr int kWidth = 256, kHeight = 192;
int target_width(int scale, bool widescreen) {
    return widescreen ? (kWidth * scale * 4 + 1) / 3 : kWidth * scale;
}
// Godot's ArrayMesh/RenderingServer allows at most 256 surfaces per mesh.
constexpr std::size_t kSurfaceLimit = 256;
constexpr std::uint32_t kMaxOverrideDimension = 2048;
constexpr std::size_t kMaxOverrideBytes = 2048U * 2048U * 4U;
constexpr std::uint64_t kMaxOverrideFileBytes = 20U * 1024U * 1024U;
constexpr std::size_t kMaxOverrideCacheBytes = 64U * 1024U * 1024U;
DqGpuScene* live_scenes = nullptr; // Main-thread-only; no static Godot resources.

SubViewport* new_viewport(Node& parent, const String& name, int scale, bool widescreen) {
    auto* viewport = memnew(SubViewport);
    viewport->set_name(name);
    viewport->set_size(Vector2i(target_width(scale, widescreen), kHeight * scale));
    viewport->set_transparent_background(true);
    viewport->set_use_own_world_3d(true);
    viewport->set_msaa_3d(Viewport::MSAA_DISABLED);
    viewport->set_update_mode(SubViewport::UPDATE_ALWAYS);
    parent.add_child(viewport);
    auto* camera = memnew(Camera3D);
    camera->set_position(Vector3(0, 0, 6));
    camera->set_projection(Camera3D::PROJECTION_ORTHOGONAL);
    camera->set_size(10); camera->set_near(0.1); camera->set_far(100);
    viewport->add_child(camera);
    camera->set_current(true);
    return viewport;
}

MeshInstance3D* new_instance(SubViewport& viewport, const String& name) {
    auto* instance = memnew(MeshInstance3D);
    instance->set_name(name);
    instance->set_custom_aabb(AABB(Vector3(-1e8, -1e8, -1e8), Vector3(2e8, 2e8, 2e8)));
    instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
    viewport.add_child(instance);
    return instance;
}

std::uint8_t expand_rgb555(std::uint32_t value, unsigned shift) {
    const auto channel = (value >> shift) & 31U;
    const auto rgb6 = channel == 0 ? 0 : channel * 2U + 1U;
    return static_cast<std::uint8_t>((rgb6 << 2U) | (rgb6 >> 4U));
}

// The worker snapshots the exact raw content while the source VRAM is pinned.
String texture_identity(const std::vector<std::uint8_t>& data) {
    if (data.empty()) return {};
    PackedByteArray bytes;
    bytes.resize(static_cast<int64_t>(data.size()));
    std::memcpy(bytes.ptrw(), data.data(), data.size());
    Ref<HashingContext> hash;
    hash.instantiate();
    hash->start(HashingContext::HASH_SHA256);
    hash->update(bytes);
    return hash->finish().hex_encode();
}

Vector4 viewport_rect(std::uint32_t viewport) {
    const auto x = viewport & 255U;
    const auto y = (viewport >> 8U) & 255U;
    const auto width = static_cast<std::uint8_t>(((viewport >> 16U) & 255U) - x) + 1;
    const auto height = static_cast<std::uint8_t>(((viewport >> 24U) & 255U) - y) + 1;
    const int y_base = y > 191U ? static_cast<int>(y) - 255 : static_cast<int>(y);
    return Vector4(x / 256.0, y_base / 192.0, width / 256.0, height / 192.0);
}

}

DqGpuScene::DqGpuScene() : next_instance_(live_scenes) {
    if (next_instance_) next_instance_->previous_instance_ = this;
    live_scenes = this;
}

DqGpuScene::~DqGpuScene() {
    release_resources(false);
    if (previous_instance_) previous_instance_->next_instance_ = next_instance_;
    else live_scenes = next_instance_;
    if (next_instance_) next_instance_->previous_instance_ = previous_instance_;
}

void DqGpuScene::shutdown_instances() {
    for (auto* instance = live_scenes; instance; instance = instance->next_instance_)
        instance->release_resources(true);
}

void DqGpuScene::_notification(int what) {
    if (what == NOTIFICATION_PREDELETE) release_resources(true);
}

void DqGpuScene::release_resources(bool children_alive) {
    if (resources_released_) return;
    resources_released_ = true;
    valid_ = color_valid_ = false;
    if (children_alive) {
        if (coverage_viewport_) memdelete(coverage_viewport_);
        if (effects_viewport_) memdelete(effects_viewport_);
        if (viewport_) memdelete(viewport_);
    }
    effects_viewport_ = coverage_viewport_ = viewport_ = nullptr;
    post_instance_ = nullptr;
    chunks_.clear();
    materials_.clear();
    textures_.clear();
    replacements_.clear();
    replacement_bytes_ = 0;
    post_material_.unref();
    id_shader_.unref();
    opaque_shader_.unref();
    clear_shader_.unref();
    partial_shader_.unref();
    fog_texture_.unref();
    edge_texture_.unref();
    toon_texture_.unref();
    mask_texture_.unref();
    side_software_texture_.unref();
    side_eligibility_texture_.unref();
    color_transfer_.unref();
    fog_image_.unref();
    edge_image_.unref();
    toon_image_.unref();
    mask_image_.unref();
    side_software_image_.unref();
    side_eligibility_image_.unref();
    fog_bytes_.clear();
    edge_bytes_.clear();
    toon_bytes_.clear();
    mask_bytes_.clear();
    side_software_bytes_.clear();
    side_eligibility_bytes_.clear();
}

void DqGpuScene::_bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("set_scale", "scale"), &DqGpuScene::set_scale);
    godot::ClassDB::bind_method(godot::D_METHOD("set_hd_textures_enabled", "enabled"), &DqGpuScene::set_hd_textures_enabled);
    godot::ClassDB::bind_method(godot::D_METHOD("get_hd_textures_enabled"), &DqGpuScene::get_hd_textures_enabled);
    godot::ClassDB::bind_method(godot::D_METHOD("set_hd_texture_dump_enabled", "enabled"), &DqGpuScene::set_hd_texture_dump_enabled);
    godot::ClassDB::bind_method(godot::D_METHOD("get_hd_texture_dump_enabled"), &DqGpuScene::get_hd_texture_dump_enabled);
    godot::ClassDB::bind_method(godot::D_METHOD("set_widescreen_enabled", "enabled"), &DqGpuScene::set_widescreen_enabled);
    godot::ClassDB::bind_method(godot::D_METHOD("get_widescreen_enabled"), &DqGpuScene::get_widescreen_enabled);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::BOOL, "widescreen_enabled"),
                 "set_widescreen_enabled", "get_widescreen_enabled");
    godot::ClassDB::bind_method(godot::D_METHOD("set_guest_widescreen", "enabled"), &DqGpuScene::set_guest_widescreen);
    godot::ClassDB::bind_method(godot::D_METHOD("get_guest_widescreen"), &DqGpuScene::get_guest_widescreen);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::BOOL, "guest_widescreen"),
                 "set_guest_widescreen", "get_guest_widescreen");
    godot::ClassDB::bind_method(godot::D_METHOD("get_texture"), &DqGpuScene::get_texture);
    godot::ClassDB::bind_method(godot::D_METHOD("get_mask_texture"), &DqGpuScene::get_mask_texture);
    godot::ClassDB::bind_method(godot::D_METHOD("get_raw_texture"), &DqGpuScene::get_raw_texture);
    godot::ClassDB::bind_method(godot::D_METHOD("get_raw_mask_texture"), &DqGpuScene::get_raw_mask_texture);
    godot::ClassDB::bind_method(godot::D_METHOD("get_screen_is_top"), &DqGpuScene::get_screen_is_top);
    godot::ClassDB::bind_method(godot::D_METHOD("get_stats"), &DqGpuScene::get_stats);
    #ifdef DQ9_GPU_SCENE_TESTS
    godot::ClassDB::bind_method(godot::D_METHOD("get_reference_image"), &DqGpuScene::get_reference_image);
    godot::ClassDB::bind_method(godot::D_METHOD("get_reference_polygon_ids"), &DqGpuScene::get_reference_polygon_ids);
    godot::ClassDB::bind_method(godot::D_METHOD("test_texture_info", "texture_param", "palette", "generation"), &DqGpuScene::test_texture_info);
    #endif
}

void DqGpuScene::set_scale(int scale) {
    scale = std::clamp(scale, 1, 6);
    if (scale_ == scale) return;
    scale_ = scale;
    if (viewport_) viewport_->set_size(godot::Vector2i(target_width(scale_, widescreen_enabled_), kHeight * scale_));
    if (effects_viewport_) effects_viewport_->set_size(godot::Vector2i(target_width(scale_, widescreen_enabled_), kHeight * scale_));
    if (coverage_viewport_) coverage_viewport_->set_size(godot::Vector2i(target_width(scale_, widescreen_enabled_), kHeight * scale_));
    const double ratio = static_cast<double>(target_width(scale_, widescreen_enabled_)) / (kWidth * scale_);
    if (post_material_.is_valid()) post_material_->set_shader_parameter("native_width_ratio", ratio);
    for (const auto& [key, material] : materials_) {
        material->set_shader_parameter("span_margin", scale_ == 1 ? 0.0F : 1.0F / 32);
        material->set_shader_parameter("side_native_width_ratio", ratio);
    }
}

void DqGpuScene::set_widescreen_enabled(bool enabled) {
    if (widescreen_enabled_ == enabled) return;
    widescreen_enabled_ = enabled;
    const godot::Vector2i size(target_width(scale_, enabled), kHeight * scale_);
    if (viewport_) viewport_->set_size(size);
    if (coverage_viewport_) coverage_viewport_->set_size(size);
    if (effects_viewport_) effects_viewport_->set_size(size);
}

bool DqGpuScene::initialize_viewport() {
    if (viewport_) return true;
    const godot::Ref<godot::Shader> source = godot::ResourceLoader::get_singleton()->load("res://shaders/ds_poly.gdshader");
    if (source.is_null()) {
        godot::UtilityFunctions::push_error("DqGpuScene requires res://shaders/ds_poly.gdshader");
        return false;
    }
    opaque_shader_.instantiate();
    opaque_shader_->set_code("#define DS_OPAQUE\n" + source->get_code());
    clear_shader_ = source;
    partial_shader_ = godot::ResourceLoader::get_singleton()->load("res://shaders/ds_translucent.gdshader");
    if (partial_shader_.is_null()) return false;
    viewport_ = new_viewport(*this, "Gpu3DViewport", scale_, widescreen_enabled_);
    ensure_chunk(0);
    mask_bytes_.resize(kWidth * kHeight);
    toon_bytes_.resize(32 * 4);
    mask_image_ = godot::Image::create(kWidth, kHeight, false, godot::Image::FORMAT_R8);
    toon_image_ = godot::Image::create(32, 1, false, godot::Image::FORMAT_RGBA8);
    mask_texture_ = godot::ImageTexture::create_from_image(mask_image_);
    toon_texture_ = godot::ImageTexture::create_from_image(toon_image_);
    static const auto transfer = [] {
        std::array<float, 256> table{};
        for (unsigned i = 1; i < table.size(); ++i) {
            const double linear = std::pow((i / 255.0 + 0.055) / 1.055, 2.4);
            double low = 0, high = 1;
            for (unsigned step = 0; step < 40; ++step) {
                const double v = (low + high) * 0.5;
                const double decoded = v * (v * (v * 0.305306011 + 0.682171111) + 0.012522878);
                if (decoded < linear) low = v; else high = v;
            }
            table[i] = static_cast<float>((low + high) * 0.5);
        }
        return table;
    }();
    godot::PackedByteArray transfer_bytes;
    transfer_bytes.resize(sizeof(transfer));
    std::memcpy(transfer_bytes.ptrw(), transfer.data(), sizeof(transfer));
    color_transfer_ = godot::ImageTexture::create_from_image(
        godot::Image::create_from_data(256, 1, false, godot::Image::FORMAT_RF, transfer_bytes));
    return true;
}

void DqGpuScene::clear_meshes() {
    surface_count_ = 0;
    for (auto& chunk : chunks_) {
        chunk.mesh->clear_surfaces();
        chunk.color->set_visible(false);
        if (chunk.metadata) chunk.metadata->set_visible(false);
        if (chunk.coverage) chunk.coverage->set_visible(false);
    }
}

void DqGpuScene::ensure_chunk(std::size_t index) {
    if (index < chunks_.size()) return;
    MeshChunk chunk;
    chunk.mesh.instantiate();
    chunk.color = new_instance(*viewport_, index == 0 ? godot::String("Gpu3DPolygons") :
                              godot::String("Gpu3DPolygons") + godot::String::num_int64(index));
    chunk.color->set_mesh(chunk.mesh);
    if (effects_viewport_) {
        chunk.metadata = new_instance(*effects_viewport_, "Gpu3DIds" + godot::String::num_int64(index));
        chunk.metadata->set_mesh(chunk.mesh);
    }
    if (coverage_viewport_) {
        chunk.coverage = new_instance(*coverage_viewport_, "Gpu3DCoverage" + godot::String::num_int64(index));
        chunk.coverage->set_mesh(chunk.mesh);
    }
    chunks_.push_back(std::move(chunk));
}

bool DqGpuScene::initialize_effects() {
    if (effects_viewport_) return true;
    id_shader_ = godot::ResourceLoader::get_singleton()->load("res://shaders/ds_poly_id.gdshader");
    const godot::Ref<godot::Shader> post_shader = godot::ResourceLoader::get_singleton()->load("res://shaders/ds_post.gdshader");
    if (id_shader_.is_null() || post_shader.is_null()) return false;
    effects_viewport_ = new_viewport(*this, "Gpu3DEffects", scale_, widescreen_enabled_);
    coverage_viewport_ = new_viewport(*this, "Gpu3DCoverage", scale_, widescreen_enabled_);
    for (std::size_t i = 0; i < chunks_.size(); ++i) {
        chunks_[i].metadata = new_instance(*effects_viewport_, "Gpu3DIds" + godot::String::num_int64(i));
        chunks_[i].metadata->set_mesh(chunks_[i].mesh);
        chunks_[i].coverage = new_instance(*coverage_viewport_, "Gpu3DCoverage" + godot::String::num_int64(i));
        chunks_[i].coverage->set_mesh(chunks_[i].mesh);
    }
    fog_bytes_.resize(32); edge_bytes_.resize(8 * 4);
    fog_image_ = godot::Image::create(32, 1, false, godot::Image::FORMAT_R8);
    edge_image_ = godot::Image::create(8, 1, false, godot::Image::FORMAT_RGBA8);
    fog_texture_ = godot::ImageTexture::create_from_image(fog_image_);
    edge_texture_ = godot::ImageTexture::create_from_image(edge_image_);
    post_material_.instantiate();
    post_material_->set_shader(post_shader);
    post_material_->set_render_priority(127);
    post_material_->set_shader_parameter("color_texture", viewport_->get_texture());
    post_material_->set_shader_parameter("coverage_texture", coverage_viewport_->get_texture());
    post_material_->set_shader_parameter("fog_table", fog_texture_);
    post_material_->set_shader_parameter("edge_color_table", edge_texture_);
    post_material_->set_shader_parameter("color_transfer", color_transfer_);
    post_material_->set_shader_parameter("display_rgb555", true);
    post_material_->set_shader_parameter("opaque_mask_output", true);
    godot::Ref<godot::QuadMesh> quad;
    quad.instantiate();
    quad->set_size(godot::Vector2(2, 2));
    quad->set_material(post_material_);
    post_instance_ = new_instance(*effects_viewport_, "Gpu3DPost");
    post_instance_->set_mesh(quad);
    return true;
}

void DqGpuScene::add_surface(const godot::Array& arrays, const dq9::GpuSceneFrame& frame,
                             const dsr::Gpu3DPolygon& polygon, bool translucent, bool span_mesh) {
    const auto index = surface_count_ / kSurfaceLimit;
    ensure_chunk(index);
    auto& chunk = chunks_[index];
    chunk.color->set_visible(true);
    const auto flags = span_mesh ?
        (static_cast<std::uint64_t>(godot::Mesh::ARRAY_CUSTOM_RGB_FLOAT) << godot::Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT) |
        (static_cast<std::uint64_t>(godot::Mesh::ARRAY_CUSTOM_RGB_FLOAT) << godot::Mesh::ARRAY_FORMAT_CUSTOM1_SHIFT) : 0U;
    chunk.mesh->add_surface_from_arrays(godot::Mesh::PRIMITIVE_TRIANGLES, arrays, {}, {}, flags);
    const auto surface = chunk.mesh->get_surface_count() - 1;
    // Chunk priorities fit even with two surfaces per polygon: at most seventeen
    // chunks including clear. Eligible partial fragments never overlap.
    chunk.mesh->surface_set_material(surface, make_material(frame, polygon, translucent, false, index, span_mesh));
    if (effects_active_) {
        chunk.metadata->set_visible(true);
        chunk.metadata->set_surface_override_material(surface, make_material(frame, polygon, translucent, true, index, span_mesh));
        if (widescreen_enabled_) {
            chunk.coverage->set_visible(true);
            chunk.coverage->set_surface_override_material(surface, make_material(frame, polygon, translucent, true, index, span_mesh, true));
        }
    }
    ++surface_count_;
}

void DqGpuScene::prepare_texture(CachedTexture& texture) {
    if (!hd_textures_enabled_ || texture.identity.is_empty()) {
        texture.image = godot::ImageTexture::create_from_image(texture.original);
        return;
    }

    if (const auto found = replacements_.find(texture.identity); found != replacements_.end()) {
        texture.image = found->second.is_valid() ? found->second :
            godot::ImageTexture::create_from_image(texture.original);
        return;
    }
    godot::Ref<godot::ImageTexture> selected;
    const godot::String path = godot::String("user://texture_overrides/") + texture.identity + godot::String(".png");
    if (godot::FileAccess::file_exists(path)) {
        // Read a bounded file once, then inspect and decode those same bytes.
        const auto file = godot::FileAccess::open(path, godot::FileAccess::READ);
        const std::uint64_t length = file.is_valid() ? file->get_length() : 0;
        const auto encoded = length >= 29 && length <= kMaxOverrideFileBytes ?
            file->get_buffer(static_cast<int64_t>(length)) : godot::PackedByteArray();
        constexpr std::uint8_t prefix[] = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82};
        // Restrict channel depth to bound the decoder's intermediate image.
        bool valid = encoded.size() == static_cast<int64_t>(length) && length >= 29 &&
            std::memcmp(encoded.ptr(), prefix, sizeof(prefix)) == 0 &&
            encoded[24] <= 8;
        std::uint32_t width = 0, height = 0;
        if (valid) {
            for (int i = 16; i < 20; ++i) width = (width << 8U) | encoded[i];
            for (int i = 20; i < 24; ++i) height = (height << 8U) | encoded[i];
            valid = width > 0 && height > 0 && width <= kMaxOverrideDimension && height <= kMaxOverrideDimension;
        }
        const std::size_t decoded_bytes = static_cast<std::size_t>(width) * height * 4U;
        valid = valid && decoded_bytes <= kMaxOverrideBytes &&
            decoded_bytes <= kMaxOverrideCacheBytes - replacement_bytes_;
        godot::Ref<godot::Image> replacement;
        if (valid) {
            replacement.instantiate();
            valid = replacement->load_png_from_buffer(encoded) == godot::OK && !replacement->is_empty() &&
                replacement->get_width() == static_cast<int>(width) &&
                replacement->get_height() == static_cast<int>(height);
        }
        if (valid) {
            replacement->convert(godot::Image::FORMAT_RGBA8);
            const auto pixels = replacement->get_data();
            const auto original = texture.original->get_data();
            const auto has_partial = [](const godot::PackedByteArray& data) {
                for (int64_t i = 3; i < data.size(); i += 4)
                    if (data[i] != 0 && data[i] != 255) return true;
                return false;
            };
            // Partial-fragment eligibility is derived from the original DS
            // pixels. Preserve its alpha exactly, including opaque and zero
            // pixels, whenever either image has fractional alpha.
            if (has_partial(pixels) || has_partial(original)) {
                const auto ow = static_cast<std::uint32_t>(texture.original->get_width());
                const auto oh = static_cast<std::uint32_t>(texture.original->get_height());
                valid = width % ow == 0 && height % oh == 0;
                if (valid) {
                    const auto sx = width / ow, sy = height / oh;
                    for (std::uint32_t y = 0; valid && y < height; ++y)
                        for (std::uint32_t x = 0; x < width; ++x)
                            if (pixels[(static_cast<int64_t>(y) * width + x) * 4 + 3] !=
                                original[(static_cast<int64_t>(y / sy) * ow + x / sx) * 4 + 3]) {
                                valid = false;
                                break;
                            }
                }
            }
        }
        if (valid) {
            selected = godot::ImageTexture::create_from_image(replacement);
            replacement_bytes_ += decoded_bytes;
        } else {
            godot::UtilityFunctions::push_warning("Invalid or unsupported texture override: ", path);
        }
    }
    replacements_.emplace(texture.identity, selected);
    texture.image = selected.is_valid() ? selected : godot::ImageTexture::create_from_image(texture.original);
}

void DqGpuScene::dump_texture(CachedTexture& texture) {
    if (texture.identity.is_empty()) return;
    const godot::String directory = "user://texture_dumps";
    const godot::String path = directory + godot::String("/") + texture.identity + godot::String(".png");
    if (godot::FileAccess::file_exists(path)) return;
    const auto error = godot::DirAccess::make_dir_recursive_absolute(directory);
    if (error != godot::OK || texture.original->save_png(path) != godot::OK)
        godot::UtilityFunctions::push_warning("Could not dump texture: ", path);
}

void DqGpuScene::set_hd_textures_enabled(bool enabled) {
    if (hd_textures_enabled_ == enabled) return;
    hd_textures_enabled_ = enabled;
    replacements_.clear();
    replacement_bytes_ = 0;
    for (auto& [key, texture] : textures_) prepare_texture(texture);
    materials_.clear();
}

void DqGpuScene::set_hd_texture_dump_enabled(bool enabled) {
    if (hd_texture_dump_enabled_ == enabled) return;
    hd_texture_dump_enabled_ = enabled;
    if (enabled) for (auto& [key, texture] : textures_) dump_texture(texture);
}

void DqGpuScene::upload_texture(const dq9::DecodedTexture& texture) {
    if (textures_.contains(texture.key)) return;
    godot::PackedByteArray bytes;
    bytes.resize(static_cast<int64_t>(texture.rgba.size()));
    std::memcpy(bytes.ptrw(), texture.rgba.data(), texture.rgba.size());
    CachedTexture cached;
    cached.original = godot::Image::create_from_data(texture.width, texture.height, false, godot::Image::FORMAT_RGBA8, bytes);
    cached.identity = texture_identity(texture.identity_data);
    prepare_texture(cached);
    if (hd_texture_dump_enabled_) dump_texture(cached);
    textures_.emplace(texture.key, std::move(cached));
}

#ifdef DQ9_GPU_SCENE_TESTS
godot::Dictionary DqGpuScene::test_texture_info(std::int64_t texture_param, std::int64_t palette,
                                                 std::int64_t generation) {
    const dsr::gpu3d_detail::TextureKey key{static_cast<std::uint32_t>(texture_param),
        static_cast<std::uint32_t>(palette), static_cast<std::uint64_t>(generation)};
    godot::Dictionary result;
    const auto it = textures_.find(key);
    if (it != textures_.end()) {
        result["identity"] = it->second.identity;
        result["image"] = it->second.image->get_image();
        result["resource_id"] = static_cast<std::int64_t>(it->second.image->get_instance_id());
    }
    return result;
}
#endif

bool DqGpuScene::same_run(const dsr::Gpu3DPolygon& a, const dsr::Gpu3DPolygon& b,
                         std::uint32_t disp3dcnt) const {
    return a.texture == b.texture && a.palette == b.palette && a.viewport == b.viewport &&
           dsr::gpu3d_detail::texture_polygon_group_key(a.texture, a.attr, disp3dcnt) ==
           dsr::gpu3d_detail::texture_polygon_group_key(b.texture, b.attr, disp3dcnt);
}

godot::Ref<godot::ShaderMaterial> DqGpuScene::make_material(const dq9::GpuSceneFrame& frame,
                                                          const dsr::Gpu3DPolygon& polygon,
                                                          bool translucent, bool metadata, int priority, bool span_mesh,
                                                          bool coverage) {
    const MaterialKey cache_key{{polygon.texture, polygon.palette, frame.vram_generation},
        dsr::gpu3d_detail::texture_polygon_group_key(polygon.texture, polygon.attr, frame.disp3dcnt),
        polygon.viewport, (priority << 4) | (translucent ? 1 : 0) | (metadata ? 2 : 0) | (span_mesh ? 4 : 0) | (coverage ? 8 : 0)};
    if (const auto found = materials_.find(cache_key); found != materials_.end()) {
        found->second->set_shader_parameter("alpha_test_ref", static_cast<int>(frame.alpha_test_ref & 31U));
        found->second->set_shader_parameter("display_rgb555", !metadata && !effects_active_);
        if (!span_mesh)
            found->second->set_shader_parameter("side_native_width_ratio",
                static_cast<double>(target_width(scale_, widescreen_enabled_)) / (kWidth * scale_));
        return found->second;
    }
    godot::Ref<godot::ShaderMaterial> material;
    material.instantiate();
    material->set_shader(metadata ? id_shader_ :
        translucent ? (span_mesh ? partial_shader_ : clear_shader_) : opaque_shader_);
    material->set_render_priority(priority);
    if (metadata) {
        material->set_shader_parameter("vertex_metadata", true);
        material->set_shader_parameter("discard_partial", span_mesh && !coverage);
        material->set_shader_parameter("discard_surface", span_mesh && translucent && !coverage);
        material->set_shader_parameter("coverage_only", coverage);
    }
    const bool textured = (frame.disp3dcnt & 1U) != 0 && ((polygon.texture >> 26U) & 7U) != 0;
    if (textured) {
        const dsr::gpu3d_detail::TextureKey key{polygon.texture, polygon.palette, frame.vram_generation};
        material->set_shader_parameter("ds_texture", textures_.at(key).image);
    }
    material->set_shader_parameter("toon_table", toon_texture_);
    material->set_shader_parameter("color_transfer", color_transfer_);
    material->set_shader_parameter("display_rgb555", !metadata && !effects_active_);
    material->set_shader_parameter("textured", textured);
    material->set_shader_parameter("polygon_mode", static_cast<int>((polygon.attr >> 4U) & 3U));
    material->set_shader_parameter("highlight", (frame.disp3dcnt & (1U << 1U)) != 0);
    material->set_shader_parameter("polygon_alpha", static_cast<int>((polygon.attr >> 16U) & 31U));
    material->set_shader_parameter("alpha_test_enabled", (frame.disp3dcnt & (1U << 2U)) != 0);
    material->set_shader_parameter("alpha_test_ref", static_cast<int>(frame.alpha_test_ref & 31U));
    material->set_shader_parameter("texture_repeat", static_cast<int>((polygon.texture >> 16U) & 3U));
    material->set_shader_parameter("texture_flip", static_cast<int>((polygon.texture >> 18U) & 3U));
    material->set_shader_parameter("viewport_rect", viewport_rect(polygon.viewport));
    material->set_shader_parameter("span_mesh", span_mesh);
    material->set_shader_parameter("span_margin", scale_ == 1 ? 0.0F : 1.0F / 32);
    material->set_shader_parameter("clear_plane", !span_mesh);
    if (!span_mesh)
        material->set_shader_parameter("side_native_width_ratio",
            static_cast<double>(target_width(scale_, widescreen_enabled_)) / (kWidth * scale_));
    materials_.emplace(cache_key, material);
    return material;
}

void DqGpuScene::apply_frame(const dq9::GpuSceneFrame& frame) {
    number_ = frame.number;
    source_frame_ = frame.source_frame;
    polygon_count_ = frame.polygon_count;
    captured_polygon_count_ = frame.captured_polygon_count;
    vertex_count_ = frame.vertex_count;
    decoded_this_frame_ = frame.decoded_this_frame;
    screen_is_top_ = frame.screen_is_top;
    three_d_pixels_ = frame.three_d_pixels;
    gpu_pixels_ = frame.gpu_pixels;
    valid_ = false;
    color_valid_ = false;
    side_valid_ = false;
    fallback_ = frame.fallback;
#ifdef DQ9_GPU_SCENE_TESTS
    reference_output_ = frame.software_output;
    reference_polygon_ids_ = frame.software_polygon_ids;
    reference_registers_ = {frame.disp3dcnt, frame.clear_color, frame.fog_color};
    reference_snapshot_counts_ = {frame.captured_snapshots, frame.valid_snapshots};
    reference_alpha_counts_ = {frame.native_translucent_polygons, frame.blending_polygons};
    reference_cause_pixels_ = frame.capture_cause_pixels;
    reference_top_polygons_ = frame.capture_top_polygons;
#endif
    mesh_polygon_count_ = skipped_wireframe_ = 0;
    wide_crossing_left_ = wide_crossing_right_ = 0;
    if (widescreen_enabled_) {
        for (std::size_t i = 0; i < frame.polygon_count; ++i) {
            const auto& polygon = frame.polygons[i];
            bool left_out = false, left_in = false, right_out = false, right_in = false;
            for (std::size_t j = 0; j < polygon.count && j < polygon.index.size(); ++j) {
                const auto index = polygon.index[j];
                if (index >= frame.vertex_count) continue;
                const auto& clip = frame.vertices[index].clip;
                left_out |= clip[0] < -clip[3];
                left_in |= clip[0] >= -clip[3];
                right_out |= clip[0] > clip[3];
                right_in |= clip[0] <= clip[3];
            }
            wide_crossing_left_ += left_out && left_in;
            wide_crossing_right_ += right_out && right_in;
        }
    }
    if (!have_generation_ || generation_ != frame.vram_generation || epoch_ != frame.epoch) {
        textures_.clear();
        replacements_.clear();
        replacement_bytes_ = 0;
        materials_.clear();
        generation_ = frame.vram_generation;
        epoch_ = frame.epoch;
        have_generation_ = true;
    }
    for (const auto& texture : frame.new_textures) upload_texture(*texture);
    if (!frame.color_valid) {
        clear_meshes();
        return;
    }
    if (!initialize_viewport()) return;
    // The post viewport also performs final RGB555 quantization after the
    // integer single-layer blend, even when fog/edge marking is disabled.
    effects_active_ = frame.gpu_valid;
    if (effects_active_ && !initialize_effects()) return;
    if (effects_viewport_) {
        effects_viewport_->set_update_mode(effects_active_ ? godot::SubViewport::UPDATE_ALWAYS : godot::SubViewport::UPDATE_DISABLED);
        coverage_viewport_->set_update_mode(effects_active_ && widescreen_enabled_ ? godot::SubViewport::UPDATE_ALWAYS : godot::SubViewport::UPDATE_DISABLED);
        post_instance_->set_visible(effects_active_);
    }
    if (effects_active_) {
        std::memcpy(fog_bytes_.ptrw(), frame.fog_table.data(), 32);
        fog_image_->set_data(32, 1, false, godot::Image::FORMAT_R8, fog_bytes_);
        fog_texture_->update(fog_image_);
        auto* edge = edge_bytes_.ptrw();
        for (std::size_t i = 0; i < 8; ++i) {
            for (unsigned c = 0; c < 3; ++c) edge[i * 4 + c] = expand_rgb555(frame.edge_color_table[i], c * 5);
            edge[i * 4 + 3] = 255;
        }
        edge_image_->set_data(8, 1, false, godot::Image::FORMAT_RGBA8, edge_bytes_);
        edge_texture_->update(edge_image_);
        post_material_->set_shader_parameter("disp3dcnt", static_cast<int>(frame.disp3dcnt));
        post_material_->set_shader_parameter("fog_color", static_cast<int>(frame.fog_color));
        post_material_->set_shader_parameter("fog_offset", static_cast<int>(frame.fog_offset));
        post_material_->set_shader_parameter("clear_id", static_cast<int>((frame.clear_color >> 24U) & 63U));
        post_material_->set_shader_parameter("clear_color", static_cast<int>(frame.clear_color));
        post_material_->set_shader_parameter("extend_side_backdrop", widescreen_enabled_ && ((frame.clear_color >> 16U) & 31U) != 0);
        post_material_->set_shader_parameter("native_width_ratio", static_cast<double>(target_width(scale_, widescreen_enabled_)) / (kWidth * scale_));
        post_material_->set_shader_parameter("clear_depth", static_cast<int>(frame.clear_depth & 0x7fffU));
    }
    auto* toon = toon_bytes_.ptrw();
    for (std::size_t i = 0; i < frame.toon_table.size(); ++i) {
        for (unsigned c = 0; c < 3; ++c) toon[i * 4 + c] = expand_rgb555(frame.toon_table[i], c * 5);
        toon[i * 4 + 3] = 255;
    }
    toon_image_->set_data(32, 1, false, godot::Image::FORMAT_RGBA8, toon_bytes_);
    toon_texture_->update(toon_image_);
    std::memcpy(mask_bytes_.ptrw(), frame.mask.data(), frame.mask.size());
    mask_image_->set_data(kWidth, kHeight, false, godot::Image::FORMAT_R8, mask_bytes_);
    mask_texture_->update(mask_image_);
    if (frame.side_software.size() == kWidth * kHeight * 4U) {
        if (side_software_texture_.is_null()) {
            side_software_bytes_.resize(kWidth * kHeight * 4);
            side_eligibility_bytes_.resize(kWidth * kHeight);
            side_software_image_ = godot::Image::create(kWidth, kHeight, false, godot::Image::FORMAT_RGBA8);
            side_eligibility_image_ = godot::Image::create(kWidth, kHeight, false, godot::Image::FORMAT_R8);
            side_software_texture_ = godot::ImageTexture::create_from_image(side_software_image_);
            side_eligibility_texture_ = godot::ImageTexture::create_from_image(side_eligibility_image_);
        }
        std::memcpy(side_software_bytes_.ptrw(), frame.side_software.data(), frame.side_software.size());
        auto* eligibility = side_eligibility_bytes_.ptrw();
        for (std::size_t i = 0; i < frame.opaque_only.size(); ++i)
            eligibility[i] = frame.opaque_only[i] ? 255 : 0;
        side_software_image_->set_data(kWidth, kHeight, false, godot::Image::FORMAT_RGBA8, side_software_bytes_);
        side_eligibility_image_->set_data(kWidth, kHeight, false, godot::Image::FORMAT_R8, side_eligibility_bytes_);
        side_software_texture_->update(side_software_image_);
        side_eligibility_texture_->update(side_eligibility_image_);
        side_valid_ = true;
    }
    clear_meshes();
    const auto clear_alpha = (frame.clear_color >> 16U) & 31U;
    if (clear_alpha != 0) {
        dsr::Gpu3DPolygon clear{};
        clear.attr = (clear_alpha << 16U) | 0xc0U | (frame.clear_color & (1U << 15U));
        clear.viewport = 0xbfff0000U;
        const auto depth = ((frame.clear_depth & 0x7fffU) << 9U) | 0x1ffU;
        const float z = depth / 8388607.5F - 1.0F;
        godot::PackedVector3Array positions;
        positions.resize(4);
        positions.set(0, godot::Vector3(-1, -1, z)); positions.set(1, godot::Vector3(1, -1, z));
        positions.set(2, godot::Vector3(1, 1, z)); positions.set(3, godot::Vector3(-1, 1, z));
        godot::PackedVector2Array uv, uv2;
        uv.resize(4); uv2.resize(4);
        const auto metadata = ((frame.clear_color >> 24U) & 63U) |
            ((frame.clear_color & (1U << 15U)) != 0 ? 64U : 0U) | (clear_alpha << 8U);
        godot::PackedColorArray colors;
        colors.resize(4);
        const auto channel = [&](unsigned shift) {
            const auto v = (frame.clear_color >> shift) & 31U;
            return (v == 0 ? 0 : v * 2U + 1U) / 63.0F;
        };
        for (int i = 0; i < 4; ++i) {
            uv2.set(i, godot::Vector2(1, metadata));
            colors.set(i, godot::Color(channel(0), channel(5), channel(10), 1));
        }
        godot::PackedInt32Array triangles;
        triangles.resize(6);
        const std::array<int, 6> order{0, 1, 2, 0, 2, 3};
        std::copy(order.begin(), order.end(), triangles.ptrw());
        godot::Array arrays;
        arrays.resize(godot::Mesh::ARRAY_MAX);
        arrays[godot::Mesh::ARRAY_VERTEX] = positions; arrays[godot::Mesh::ARRAY_COLOR] = colors;
        arrays[godot::Mesh::ARRAY_TEX_UV] = uv; arrays[godot::Mesh::ARRAY_TEX_UV2] = uv2;
        arrays[godot::Mesh::ARRAY_INDEX] = triangles;
        add_surface(arrays, frame, clear, clear_alpha != 31U);
    }
    for (std::size_t begin = 0; begin < frame.polygon_count;) {
        const bool translucent = begin >= frame.opaque_count;
        std::size_t end = begin + 1;
        while (end < frame.polygon_count && (end >= frame.opaque_count) == translucent &&
               same_run(frame.polygons[begin], frame.polygons[end], frame.disp3dcnt)) ++end;
        const auto& polygon = frame.polygons[begin];
        const auto alpha = (polygon.attr >> 16U) & 31U;
        const auto format = (polygon.texture >> 26U) & 7U;
        const bool partial_run = (frame.disp3dcnt & 8U) != 0U &&
            (polygon.attr & ((1U << 11U) | (1U << 14U))) == 0U &&
            ((frame.disp3dcnt & 4U) == 0U ||
             ((frame.alpha_test_ref & 31U) < 31U && alpha >= (frame.alpha_test_ref & 31U))) &&
            (alpha != 31U || ((frame.disp3dcnt & 1U) != 0U &&
             ((polygon.attr >> 4U) & 3U) != 1U && (format == 1U || format == 6U)));
        if (alpha == 0U || ((polygon.attr >> 4U) & 3U) == 3U ||
            (alpha != 31U && !partial_run)) {
            if (alpha == 0U) skipped_wireframe_ += end - begin;
            begin = end;
            continue;
        }
        raster_vertices_.clear();
        for (std::size_t i = begin; i < end; ++i)
            if (raster_mesh_.append(frame, frame.polygons[i], translucent, raster_vertices_, widescreen_enabled_ && !guest_widescreen_))
                ++mesh_polygon_count_;
        const auto vertices = raster_vertices_.size();
        if (vertices != 0) {
            godot::PackedVector3Array positions;
            godot::PackedVector2Array uv, uv2;
            godot::PackedFloat32Array colors, depth_planes;
            godot::PackedInt32Array triangles;
            positions.resize(vertices); uv.resize(vertices); uv2.resize(vertices); colors.resize(vertices * 3);
            depth_planes.resize(vertices * 3);
            triangles.resize(vertices / 4 * 6);
            auto* pos_out = positions.ptrw(); auto* uv_out = uv.ptrw(); auto* w_out = uv2.ptrw();
            auto* color_out = colors.ptrw(); auto* tri_out = triangles.ptrw();
            auto* depth_out = depth_planes.ptrw();
            const int width = 8 << ((polygon.texture >> 20U) & 7U);
            const int height = 8 << ((polygon.texture >> 23U) & 7U);
            for (std::size_t v = 0; v < vertices; ++v) {
                const auto& sample = raster_vertices_[v];
                const auto& vertex = sample.vertex;
                pos_out[v] = godot::Vector3(vertex.clip[0], vertex.clip[1], vertex.clip[2]);
                w_out[v] = godot::Vector2(vertex.clip[3], sample.metadata);
                uv_out[v] = godot::Vector2(vertex.u / width, vertex.v / height);
                for (unsigned c = 0; c < 3; ++c) color_out[v * 3 + c] = vertex.color[c] / 63.0F;
                for (unsigned c = 0; c < 3; ++c) depth_out[v * 3 + c] = sample.depth_plane[c];
            }
            for (std::size_t quad = 0; quad < vertices / 4; ++quad) {
                const auto base = static_cast<std::int32_t>(quad * 4);
                const std::array<int, 6> order{0, 1, 2, 0, 2, 3};
                for (unsigned t = 0; t < 6; ++t) tri_out[quad * 6 + t] = base + order[t];
            }
            godot::Array arrays;
            arrays.resize(godot::Mesh::ARRAY_MAX);
            arrays[godot::Mesh::ARRAY_VERTEX] = positions;
            arrays[godot::Mesh::ARRAY_TEX_UV] = uv;
            arrays[godot::Mesh::ARRAY_TEX_UV2] = uv2;
            arrays[godot::Mesh::ARRAY_CUSTOM0] = colors;
            arrays[godot::Mesh::ARRAY_CUSTOM1] = depth_planes;
            arrays[godot::Mesh::ARRAY_INDEX] = triangles;
            // Actual alpha31 fragments enter opaque depth/metadata. A second
            // pass shades partial fragments against its frozen colour snapshot.
            if (alpha == 31U) add_surface(arrays, frame, polygon, false, true);
            if (partial_run) add_surface(arrays, frame, polygon, true, true);
        }
        begin = end;
    }
    color_valid_ = true;
    valid_ = frame.gpu_valid;
    fallback_ = frame.fallback;
}

godot::Ref<godot::Texture2D> DqGpuScene::get_texture() const {
    const auto* output = effects_active_ ? effects_viewport_ : viewport_;
    return valid_ && output ? godot::Ref<godot::Texture2D>(output->get_texture()) : godot::Ref<godot::Texture2D>();
}

godot::Ref<godot::Texture2D> DqGpuScene::get_mask_texture() const {
    return valid_ ? godot::Ref<godot::Texture2D>(mask_texture_) : godot::Ref<godot::Texture2D>();
}
godot::Ref<godot::Texture2D> DqGpuScene::get_side_software_texture() const {
    return valid_ && side_valid_ ? godot::Ref<godot::Texture2D>(side_software_texture_) : godot::Ref<godot::Texture2D>();
}

godot::Ref<godot::Texture2D> DqGpuScene::get_side_eligibility_texture() const {
    return valid_ && side_valid_ ? godot::Ref<godot::Texture2D>(side_eligibility_texture_) : godot::Ref<godot::Texture2D>();
}


godot::Ref<godot::Texture2D> DqGpuScene::get_raw_texture() const {
    return color_valid_ && viewport_ ? godot::Ref<godot::Texture2D>(viewport_->get_texture()) : godot::Ref<godot::Texture2D>();
}

godot::Ref<godot::Texture2D> DqGpuScene::get_raw_mask_texture() const {
    return color_valid_ ? godot::Ref<godot::Texture2D>(mask_texture_) : godot::Ref<godot::Texture2D>();
}
#ifdef DQ9_GPU_SCENE_TESTS
godot::Ref<godot::Image> DqGpuScene::get_reference_image() const {
    if (!color_valid_) return {};
    godot::PackedByteArray bytes;
    bytes.resize(kWidth * kHeight * 4);
    auto* out = bytes.ptrw();
    for (std::size_t i = 0; i < reference_output_.size(); ++i) {
        const auto pixel = reference_output_[i];
        for (unsigned c = 0; c < 3; ++c) {
            const auto value = (pixel >> (c * 6U)) & 63U;
            out[i * 4 + c] = static_cast<std::uint8_t>((value << 2U) | (value >> 4U));
        }
        const auto alpha = (pixel >> 24U) & 31U;
        out[i * 4 + 3] = static_cast<std::uint8_t>((alpha << 3U) | (alpha >> 2U));
    }
    return godot::Image::create_from_data(kWidth, kHeight, false, godot::Image::FORMAT_RGBA8, bytes);
}

godot::PackedByteArray DqGpuScene::get_reference_polygon_ids() const {
    godot::PackedByteArray ids;
    if (color_valid_) {
        ids.resize(reference_polygon_ids_.size());
        std::copy(reference_polygon_ids_.begin(), reference_polygon_ids_.end(), ids.ptrw());
    }
    return ids;
}
#endif


godot::Dictionary DqGpuScene::get_stats() const {
    godot::Dictionary out;
    out["valid"] = valid_;
    out["color_valid"] = color_valid_;
    out["fallback"] = dq9::fallback_name(fallback_);
    out["frame"] = static_cast<int64_t>(number_);
    out["source_frame"] = static_cast<int64_t>(source_frame_);
    out["effects_active"] = effects_active_;
#ifdef DQ9_GPU_SCENE_TESTS
    out["reference_disp3dcnt"] = static_cast<int64_t>(reference_registers_[0]);
    out["reference_clear_color"] = static_cast<int64_t>(reference_registers_[1]);
    out["reference_fog_color"] = static_cast<int64_t>(reference_registers_[2]);
    out["captured_snapshots"] = static_cast<int64_t>(reference_snapshot_counts_[0]);
    out["valid_snapshots"] = static_cast<int64_t>(reference_snapshot_counts_[1]);
    out["native_translucent_polygons"] = static_cast<int64_t>(reference_alpha_counts_[0]);
    out["blending_polygons"] = static_cast<int64_t>(reference_alpha_counts_[1]);
    static constexpr const char* cause_names[] = {
        "partial_polygon_pixels", "partial_texel_pixels", "shadow_pixels",
        "wireframe_pixels", "opaque_decal_pixels"};
    for (unsigned i = 0; i < reference_cause_pixels_.size(); ++i)
        out[cause_names[i]] = static_cast<int64_t>(reference_cause_pixels_[i]);
    godot::Array top_polygons;
    for (const auto& polygon : reference_top_polygons_) {
        if (polygon[5] == 0) continue;
        godot::Dictionary entry;
        static constexpr const char* fields[] = {"index", "id", "alpha", "format", "mode", "pixels"};
        for (unsigned i = 0; i < polygon.size(); ++i)
            entry[fields[i]] = static_cast<int64_t>(polygon[i]);
        top_polygons.push_back(entry);
    }
    out["partial_top_polygons"] = top_polygons;
#endif
    out["mesh_count"] = static_cast<int64_t>((surface_count_ + kSurfaceLimit - 1) / kSurfaceLimit);
    out["polygon_count"] = static_cast<int64_t>(polygon_count_);
    out["captured_polygon_count"] = static_cast<int64_t>(captured_polygon_count_);
    out["vertex_count"] = static_cast<int64_t>(vertex_count_);
    out["mesh_polygon_count"] = static_cast<int64_t>(mesh_polygon_count_);
    out["skipped_wireframe"] = static_cast<int64_t>(skipped_wireframe_);
    out["wide_crossing_left"] = static_cast<int64_t>(wide_crossing_left_);
    out["wide_crossing_right"] = static_cast<int64_t>(wide_crossing_right_);
    out["three_d_pixels"] = static_cast<int64_t>(three_d_pixels_);
    out["gpu_pixels"] = static_cast<int64_t>(gpu_pixels_);
    out["gpu_pixel_share"] = three_d_pixels_ ? static_cast<double>(gpu_pixels_) / three_d_pixels_ : 0.0;
    out["surface_count"] = static_cast<int64_t>(surface_count_);
    out["textures_cached"] = static_cast<int64_t>(textures_.size());
    out["decoded_this_frame"] = static_cast<int64_t>(decoded_this_frame_);
    out["vram_generation"] = static_cast<int64_t>(generation_);
    out["scale"] = scale_;
    return out;
}
