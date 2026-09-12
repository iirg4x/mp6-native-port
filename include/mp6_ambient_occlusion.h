/* Port-owned screen-space shading. No gameplay state or decomp implementation. */
#ifndef MP6_AMBIENT_OCCLUSION_H
#define MP6_AMBIENT_OCCLUSION_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct Mp6AoView {
    float projection[4]; /* tan(fovY/2), aspect, near, far */
    float viewport[4];   /* x, y, width, height, normalized to the EFB */
    float scissor[4];    /* normalized x, y, width, height */
    float depthRange[2]; /* GX viewport near/far */
    int camera;          /* snapshot scope; never share decal depth across cameras */
} Mp6AoView;

void mp6_ao_camera_end(int camera); /* Hu3DExec, before front sprites */
void mp6_ao_camera_begin(int camera); /* real game camera boundary */
void mp6_ao_depth_cleared(int camera); /* after Hu3DZClear's GXEnd */
int mp6_ao_camera_has_depth(int camera); /* real-frame admission only */
void mp6_ao_decals(int camera, int after); /* brackets the original space draw */
void mp6_ao_capture_decals(int camera, int after); /* renderer + retained frames */
void mp6_ao_begin_frame(void); /* each admitted real OR interpolated frame */
void mp6_ao_apply(const Mp6AoView *view); /* real and retained-stream frames */
void mp6_ao_shutdown(void); /* before Aurora/device teardown */
void mp6_ao_foliage_render(int camera, int enabled); /* HSF alpha coverage only */
int mp6_ao_foliage_begin(int camera); /* private target, also replayed */
int mp6_ao_foliage_target_size(unsigned *width, unsigned *height);
void mp6_ao_foliage_camera(int camera); /* logical EFB -> private target pixels */
void mp6_ao_foliage_end(int camera);
/* Called AFTER resolve_pass drains GX. Retains only values + a byte boundary,
 * never a texture/host pointer. The replay builder remaps the boundary when
 * removing offscreen and copy commands. */
void mp6_fi_note_ao(const Mp6AoView *view);
void mp6_fi_note_ao_decals(int camera, int after);
void mp6_fi_note_ao_foliage(int camera, int after);

#ifdef __cplusplus
}
#endif
#endif
