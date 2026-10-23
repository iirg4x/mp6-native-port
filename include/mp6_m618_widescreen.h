#ifndef MP6_M618_WIDESCREEN_H
#define MP6_M618_WIDESCREEN_H

/* Included only by M618's port compatibility unit, after its scene globals.
 * Adjacent course sections share world space. Use an independent wall mesh
 * for extended views so an extension cannot cover another section's pipe. */
static HU3D_MODELID portWideWalls[4][2];
static u16 portWallCameras[4][2];
static HU3D_MODELID portWideBackdrops[4][3];
static u16 portBackdropCameras[4][3];

static void portWideWallsInit(void)
{
    for (int stage = 0; stage < 4; ++stage) {
        for (int part = 0; part < 3; ++part) {
            portWideBackdrops[stage][part] = HU3D_MODELID_NONE;
            portBackdropCameras[stage][part] = 15;
        }
        for (int side = 0; side < 2; ++side) {
            portWideWalls[stage][side] = HU3D_MODELID_NONE;
            portWallCameras[stage][side] = 15;
        }
    }
}

static void portWideWallCreate(int stage, int side, int file, const char *name, float boundary)
{
    /* Prepare the hidden companion even when the scene starts in 4:3.
     * portWideWallsUpdate alone chooses which cameras can see it. */
    void *data = HuDataSelHeapReadNum(file, HU_MEMNUM_OVL, HEAP_MODEL);
    if (!data) return;
    HU3D_MODELID model = Hu3DModelCreate(data);
    if (model == HU3D_MODELID_NONE) {
        HuMemDirectFree(data);
        return;
    }
    portWideWalls[stage][side] = model;
    Hu3DModelPosSet(model, lbl_1_data_48[stage].x, lbl_1_data_48[stage].y, 0.0f);
    Hu3DModelCameraSet(model, 0);
    /* Native half-width at this 10-degree / 7000-distance camera is under
     * 800, including the wall's depth. Preserve brick density and pipe size. */
    mp6_widescreen_extend_edge_span(model, name, 0, boundary, 800.0f);
}

static void portWideBackdropCreate(int stage, int part, int file, const char *name,
                                   int axis, float centerX, float centerOther)
{
    void *data = HuDataSelHeapReadNum(file, HU_MEMNUM_OVL, HEAP_MODEL);
    if (!data) return;
    HU3D_MODELID model = Hu3DModelCreate(data);
    if (model == HU3D_MODELID_NONE) { HuMemDirectFree(data); return; }
    portWideBackdrops[stage][part] = model;
    Hu3DModelPosSet(model, lbl_1_data_48[stage].x, lbl_1_data_48[stage].y, 0.0f);
    Hu3DModelCameraSet(model, 0);
    Hu3DModelAttrSet(model, HU3D_MOTATTR_LOOP);
    if (axis >= 0)
        mp6_widescreen_extend_color_rim(model, name, axis, centerX, centerOther);
}

static void portWidePairUpdate(HU3D_MODELID wide, HU3D_MODELID native, u16 desired, u16 cameras)
{
    if (wide == HU3D_MODELID_NONE) return;
    u16 visible = cameras & desired;
    if (Hu3DData[native].attr & HU3D_ATTR_DISPOFF) Hu3DModelAttrSet(wide, HU3D_ATTR_DISPOFF);
    else Hu3DModelAttrReset(wide, HU3D_ATTR_DISPOFF);
    Hu3DModelCameraSet(native, desired & ~visible);
    Hu3DModelCameraSet(wide, visible);
}

static void portWideWallsUpdate(void)
{
    for (int stage = 0; stage < 4; ++stage) {
        u16 cameras = 0;
        if (mp6_widescreen_enabled() && mp6_widescreen_scale_factor() > 1.0f) {
            for (int player = 0; player < 4; ++player) {
                if (lbl_1_bss_114[player].stageSection == stage) cameras |= lbl_1_data_28[player];
            }
        }
        for (int side = 0; side < 2; ++side) {
            portWidePairUpdate(portWideWalls[stage][side], lbl_1_bss_2BC[stage][side],
                               portWallCameras[stage][side], cameras);
        }
        for (int part = 0; part < 3; ++part) {
            portWidePairUpdate(portWideBackdrops[stage][part], lbl_1_bss_2F4[stage][part],
                               portBackdropCameras[stage][part], cameras);
            if (stage == 3 && part == 0) {
                /* Pane edges are not repeat boundaries: the native panes
                 * leave a 14.923-unit grid bar between them. Reflect halfway
                 * through that gap, or two edge panes merge into a double
                 * width cell. All ten rows share this authored X layout. */
                mp6_widescreen_tile_mirror_x(portWideBackdrops[stage][part],
                    -301.0012f - 7.4615f, 3309.1135f + 7.4615f, 800.0f);
            }
        }
    }
}

#endif
