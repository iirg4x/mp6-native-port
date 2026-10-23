"""Widescreen fixes for Snow Brawl, Tally Me Banana, Mowtown and Pier Factor."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def added(rel):
    patch = (ROOT / 'compat/decomp' / rel).read_text(encoding='utf-8')
    return '\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+'))


class Batch2Widescreen(unittest.TestCase):
    def test_water_reflections_copy_and_project_the_live_width(self):
        for rel, copies in (('src/REL/m674Dll/effects.c.patch', 1), ('src/REL/m676Dll/effects.c.patch', 2)):
            text = added(rel)
            self.assertEqual(text.count('GXSetTexCopySrc(0, 0, mp6_widescreen_render_width(), 480);'), copies, rel)
            self.assertIn('C_MTXLightPerspective(perspectiveMatrix, fov, 1.2f * mp6_widescreen_scale_factor(),', text)

    def test_water_disc_arrays_upload_their_real_spans(self):
        # Display-list replay skips GXBegin size learning; bare GXSetArray
        # binds zero bytes and the water tint drifts with stale GPU data.
        for rel, (pos, clr, uv) in (('src/REL/m674Dll/effects.c.patch', ('6C0', '5A0', '780')),
                                    ('src/REL/m676Dll/effects.c.patch', ('800', '6E0', '8C0'))):
            text = added(rel)
            self.assertIn('#include "mp6_gxarray_registry.h"', text)
            for attr, name, stride in (('GX_VA_POS', pos, 6), ('GX_VA_CLR0', clr, 2), ('GX_VA_TEX0', uv, 4)):
                self.assertIn(f'mp6_gxarray_bind_span({attr}, lbl_1_rodata_{name}, '
                              f'sizeof(lbl_1_rodata_{name}), {stride});', text, rel)

    def test_tally_me_banana_framing_fronds_follow_the_frame_edges(self):
        text = added('src/REL/m674Dll/targets.c.patch')
        self.assertIn('mp6_widescreen_motion_x(model, frond->name,', text)
        self.assertIn("frond->name[0] == 'h' ? MP6_M674_FROND_PIVOT * frond->mesh.base.pos.x : 0.0f);", text)
        # Loose fronds (ha<n>) and their shadows (yasi_K<n>), never the trees' yasi_KB<n>.
        self.assertIn("&& name[4] == '_' && name[5] == 'K' ? name + 6 : NULL;", text)
        self.assertIn("if (*digits < '0' || *digits > '9') return 0;", text)

    def test_mowtown_results_extend_edges_instead_of_zooming(self):
        text = added('src/REL/m613Dll/bitmap.c.patch')
        self.assertNotIn('scale_backdrop', text)
        self.assertNotIn('getenv', text)  # QA diagnostics never ship
        model = 'lbl_1_bss_1C.modelIds[3]'
        for call in ('"wall", 0, 0, 1400.0f, 0.0f, 0', '"fog", 0, 0, 1400.0f, 0.0f, 0',
                     '"wall", 1, 0, 1400.0f, 0.0f, 0', '"bgSiba", 0, -1, 1000.0f, 0.0f, 1',
                     '"bgSiba1", 0, 1, 1000.0f, 0.0f, 1',
                     # Flower beds grow under their outermost clusters, capped.
                     '"plant", 0, -1, 1200.0f, 400.0f, 1', '"plant2", 0, 1, 1200.0f, 400.0f, 1'):
            self.assertIn(f'mp6_widescreen_extend_outer_x({model}, {call});', text)

    def test_mowtown_outer_flowers_get_stems(self):
        text = added('src/REL/m613Dll/bitmap.c.patch')
        model = 'lbl_1_bss_1C.modelIds[3]'
        for stem, head in (('kuki', 'fp'), ('kuki', 'fpB'), ('kuki1', 'fp2'), ('kuki1', 'fpB1')):
            self.assertIn(f'mp6_widescreen_add_stems({model}, "{stem}", "{head}", 40.0f);', text)
        source = (ROOT / 'src/hsf/mp6_widescreen_extrude.c').read_text(encoding='utf-8')
        body = source[source.index('void mp6_widescreen_add_stems('):]
        body = body[:body.index('\n}\n')]
        # Hierarchy transforms, already-seated heads skipped, tips kept below the bloom.
        self.assertIn('if (!mp6_ws_stem_object_mtx(hsf, stem, stemMtx) || !PSMTXInverse(stemMtx, stemInv)) return;', body)
        self.assertIn('seated = mp6_ws_stem_near(plan->modelTop[i], centre, headRadius);', body)
        self.assertIn('target.y = topModel[best].y < seat ? topModel[best].y : seat;', body)

    def test_dk_plaza_continues_into_its_mirror_past_the_cut(self):
        text = added('src/REL/m674Dll/targets.c.patch')
        self.assertIn('mp6_widescreen_mirror_mesh(model, "cyl21", 0, 0, 600.0f);', text)
        source = (ROOT / 'src/hsf/mp6_widescreen_extrude.c').read_text(encoding='utf-8')
        body = source[source.index('static void mp6_ws_mirror_write_faces('):]
        body = body[:body.index('\n}\n')]
        # Reflected copies keep facing outward.
        self.assertIn('tail[f].index[1] = tail[f].index[2];', body)
        self.assertIn('g_mp6WsMirrored[i].obj = NULL;', source)  # forgotten with its model

    def test_snow_brawl_sky_scales_about_its_top_edge(self):
        text = added('src/REL/m626Dll/scenery.c.patch')
        self.assertIn('mp6_widescreen_scale_backdrop(model, "bg", 1);', text)

    def test_outer_x_moves_only_edge_planes_and_restores_native(self):
        source = (ROOT / 'src/hsf/mp6_widescreen_extrude.c').read_text(encoding='utf-8')
        apply = source[source.index('static void mp6_ws_outer_x_apply('):]
        apply = apply[:apply.index('\n}\n')]
        # 4:3 gives d == 0: native X, STs and wrap mode exactly.
        self.assertIn('float d = k > 1.0f ? e->reach * (k - 1.0f) : 0.0f;', apply)
        self.assertIn('if (e->limit > 0.0f && d > e->limit) d = e->limit;', apply)
        self.assertIn('*mp6_ws_outer_x_coord(&e->vtx[i], e->axis) = e->nativeC[i] + e->side[i] * d;', apply)
        self.assertIn('e->attr->wrapS = d > 0.0f ? TRUE : e->nativeWrapS;', apply)
        body = source[source.index('void mp6_widescreen_extend_outer_x('):]
        body = body[:body.index('\n}\n')]
        self.assertIn('if (rot[0][axis] * *mp6_ws_outer_x_coord(&o->mesh.base.scale, axis) < 0.0f) sides = -sides;', body)
        self.assertIn('outer-x: ST shared with an interior corner', body)
        self.assertIn('mp6_ws_outer_x_reapply_all(k);', source)
        self.assertIn('g_mp6WsOuterX[i].obj = NULL;', source)  # forgotten with its model


if __name__ == '__main__':
    unittest.main()
