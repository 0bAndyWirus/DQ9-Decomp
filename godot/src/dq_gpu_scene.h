#pragma once

#include "gpu_scene_frame.h"
#include "ds_raster_mesh.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <unordered_map>

namespace godot { class SubViewport; class MeshInstance3D; }

class DqGpuScene final : public godot::Node {
    GDCLASS(DqGpuScene, godot::Node)
public:
    DqGpuScene();
    ~DqGpuScene() override;
    static void shutdown_instances();
    void _notification(int what);
    void set_scale(int scale);
    void apply_frame(const dq9::GpuSceneFrame& frame);
    godot::Ref<godot::Texture2D> get_texture() const;
    godot::Ref<godot::Texture2D> get_mask_texture() const;
    godot::Ref<godot::Texture2D> get_raw_texture() const;
    godot::Ref<godot::Texture2D> get_raw_mask_texture() const;
    bool get_screen_is_top() const { return screen_is_top_; }
    godot::Dictionary get_stats() const;
#ifdef DQ9_GPU_SCENE_TESTS
    godot::Ref<godot::Image> get_reference_image() const;
    godot::PackedByteArray get_reference_polygon_ids() const;
#endif

protected:
    static void _bind_methods();

private:
    void release_resources(bool children_alive);
    bool initialize_viewport();
    godot::Ref<godot::ShaderMaterial> make_material(const dq9::GpuSceneFrame& frame,
                                                   const dsr::Gpu3DPolygon& polygon,
                                                   bool translucent, bool metadata, int priority, bool span_mesh);
    bool same_run(const dsr::Gpu3DPolygon& a, const dsr::Gpu3DPolygon& b,
                  std::uint32_t disp3dcnt) const;
    void upload_texture(const dq9::DecodedTexture& texture);
    bool initialize_effects();
    void clear_meshes();
    void ensure_chunk(std::size_t index);
    void add_surface(const godot::Array& arrays, const dq9::GpuSceneFrame& frame,
                     const dsr::Gpu3DPolygon& polygon, bool translucent, bool span_mesh = false);

    godot::SubViewport* viewport_ = nullptr;
    struct MeshChunk {
        godot::Ref<godot::ArrayMesh> mesh;
        godot::MeshInstance3D* color = nullptr;
        godot::MeshInstance3D* metadata = nullptr;
    };
    std::vector<MeshChunk> chunks_;
    std::vector<dq9::RasterMeshVertex> raster_vertices_;
    dq9::DsRasterMesh raster_mesh_;
    godot::SubViewport* effects_viewport_ = nullptr;
    godot::MeshInstance3D* post_instance_ = nullptr;
    godot::Ref<godot::Shader> id_shader_;
    godot::Ref<godot::ShaderMaterial> post_material_;
    godot::Ref<godot::ImageTexture> fog_texture_, edge_texture_;
    godot::Ref<godot::Image> fog_image_, edge_image_;
    godot::PackedByteArray fog_bytes_, edge_bytes_;
    bool effects_active_ = false;
    std::size_t surface_count_ = 0;
    godot::Ref<godot::Shader> opaque_shader_, clear_shader_, partial_shader_;
    godot::Ref<godot::ImageTexture> toon_texture_, mask_texture_;
    godot::Ref<godot::ImageTexture> color_transfer_;
    godot::Ref<godot::Image> toon_image_, mask_image_;
    godot::PackedByteArray toon_bytes_, mask_bytes_;
#ifdef DQ9_GPU_SCENE_TESTS
    std::array<std::uint32_t, 256 * 192> reference_output_{};
    std::array<std::uint8_t, 256 * 192> reference_polygon_ids_{};
    std::array<std::uint32_t, 3> reference_registers_{};
    std::array<std::uint64_t, 2> reference_snapshot_counts_{};
    std::array<std::uint32_t, 2> reference_alpha_counts_{};
    std::array<std::uint32_t, 5> reference_cause_pixels_{};
    std::array<std::array<std::uint32_t, 6>, 8> reference_top_polygons_{};
#endif
    struct MaterialKey {
        dsr::gpu3d_detail::TextureKey texture;
        std::uint32_t group = 0, viewport = 0;
        int variant = 0;
        bool operator==(const MaterialKey&) const = default;
    };
    struct MaterialKeyHash {
        std::size_t operator()(const MaterialKey& key) const noexcept {
            return dq9::TextureKeyHash{}(key.texture) ^
                (std::hash<std::uint32_t>{}(key.group) << 1U) ^
                (std::hash<std::uint32_t>{}(key.viewport) << 2U) ^
                (std::hash<int>{}(key.variant) << 3U);
        }
    };
    std::unordered_map<MaterialKey, godot::Ref<godot::ShaderMaterial>, MaterialKeyHash> materials_;
    struct CachedTexture {
        godot::Ref<godot::ImageTexture> image;
    };
    std::unordered_map<dsr::gpu3d_detail::TextureKey, CachedTexture, dq9::TextureKeyHash> textures_;
    std::uint64_t generation_ = 0, epoch_ = 0, number_ = 0, source_frame_ = 0;
    bool have_generation_ = false, valid_ = false, color_valid_ = false, screen_is_top_ = true;
    int scale_ = 3;
    std::size_t polygon_count_ = 0, captured_polygon_count_ = 0, vertex_count_ = 0;
    std::size_t mesh_polygon_count_ = 0, skipped_wireframe_ = 0;
    std::uint32_t three_d_pixels_ = 0, gpu_pixels_ = 0;
    std::size_t decoded_this_frame_ = 0;
    dq9::GpuFallback fallback_ = dq9::GpuFallback::Disabled;
    DqGpuScene* previous_instance_ = nullptr;
    DqGpuScene* next_instance_ = nullptr;
    bool resources_released_ = false;
};
