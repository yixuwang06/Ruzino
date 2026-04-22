#pragma once

#include <cstdint>

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>

#include "GUI/widget.h"
#include "widgets/api.h"

RUZINO_NAMESPACE_OPEN_SCOPE
class Stage;
class NodeSystem;

class USDVIEW_WIDGET_API UsdFileViewer : public IWidget {
   public:
    explicit UsdFileViewer(Stage* stage);

    ~UsdFileViewer() override;

    bool BuildUI() override;

    std::string GetWindowUniqueName() override
    {
        return "UsdFileViewer##Singleton";
    }

   protected:
    bool Begin() override
    {
        return true;
    }

    void End() override
    {
    }

   private:
    void ShowFileTree();
    void ShowPrimInfo();
    void EditValue();
    void select_file();

    void remove_prim_logic();
    void show_right_click_menu();
    void DrawChild(const pxr::UsdPrim& prim, bool is_root = false);

    // Helper to update selection and emit event
    void set_selected_prim(const pxr::SdfPath& path);

    // Subscribe to viewport picking events
    void subscribe_to_viewport_events();

    // Material editing helpers
    bool is_material_prim(const pxr::UsdPrim& prim);
    bool is_geometry_prim(const pxr::UsdPrim& prim);
    void collect_all_materials();
    void open_material_editor(const pxr::SdfPath& material_path);
    void open_material_document_viewer(const pxr::SdfPath& material_path);
    void show_material_binding_ui(pxr::UsdPrim& prim);

    pxr::SdfPath selected;

    pxr::SdfPath to_delete;  // workaround for deleting prims. usdview has cache
                             // that cannot be safely deleted
    static int delete_pass_id;

    Stage* stage;
    bool is_selecting_file = false;
    pxr::SdfPath selecting_file_base;

    // Cached transform values to avoid recomputing from matrix every frame
    pxr::SdfPath cached_transform_path;
    pxr::GfVec3d cached_euler_angles;
    pxr::GfVec3d cached_scale;
    bool has_cached_transform = false;

    // Material editing support
    std::vector<pxr::SdfPath> opened_material_editors;
    std::vector<pxr::SdfPath> all_materials_cache;
    bool materials_cache_dirty = true;
    bool viewport_event_subscribed = false;
    std::uint64_t viewport_pick_subscription_id_ = 0;
    std::uint64_t camera_transform_subscription_id_ = 0;
};
RUZINO_NAMESPACE_CLOSE_SCOPE
