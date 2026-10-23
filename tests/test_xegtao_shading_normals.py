"""CPU checks for the isolated captured-mesh normal comparison."""
import sys
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/integration"))
from analyze_xegtao_shading_normals import Camera, clip_triangle, corner_triangles, np, rasterize


class ShadingNormalRasterTests(unittest.TestCase):
    def test_native_probe_hash_is_admitted_by_qa_runner(self):
        builder = (ROOT / "tests/integration/build_board_qa.py").read_text()
        runner = (ROOT / "tests/integration/run_board_qa.py").read_text()
        self.assertIn("'geometry_probe_sha256': hashlib.sha256", builder)
        self.assertIn("ready.get('geometry_probe_sha256') != geometry_probe_hash", runner)
        self.assertIn("root/'tests/integration/ground_geometry_probe.h'", runner)

    def camera(self):
        return Camera(dict(viewMatrix=[1,0,0,0,0,1,0,0,0,0,1,0],
                           viewport=[0,0,848,480], fov=45, aspect=1.59), (64, 36))

    def row(self):
        return dict(name="plane", objectIndex=7,
                    indexedNormalsAreFinal=True,
                    vertices=[[-200,200,-1000], [-200,-200,-1000],
                              [200,200,-1000], [200,-200,-1000]],
                    viewNormals=[[0,0,1]], faces=[[3,0,1,2,3]],
                    faceNormalIndices=[[0,0,0,0]])

    def test_native_corner_order(self):
        self.assertEqual(list(corner_triangles(2,3)), [(0,2,1)])
        self.assertEqual(list(corner_triangles(3,4)), [(0,2,3),(0,3,1)])
        self.assertEqual(list(corner_triangles(4,5)), [(0,2,1),(2,1,3),(1,3,4)])
        with self.assertRaises(ValueError):
            list(corner_triangles(1,3))

    def test_flat_quad_depth_and_forward_convention(self):
        depth, normals, owners, count = rasterize([self.row()], self.camera(), 64, 36)
        coverage = depth > 0
        self.assertTrue(coverage[18,32])
        self.assertGreater(int(coverage.sum()), 200)
        self.assertEqual(count, 2)
        np.testing.assert_allclose(depth[coverage], 1000, atol=1e-4)
        np.testing.assert_allclose(normals[coverage], np.tile([0,0,-1], (coverage.sum(),1)))
        self.assertTrue(np.all(owners[coverage] == 7))
        self.assertTrue(np.all(owners[~coverage] == -1))

    def test_nearer_geometry_wins_independent_of_order(self):
        back, front = self.row(), self.row()
        front["vertices"] = [[x,y,z*.5] for x,y,z in front["vertices"]]
        front["objectIndex"] = 8
        a = rasterize([back, front], self.camera(), 64, 36)
        b = rasterize([front, back], self.camera(), 64, 36)
        np.testing.assert_array_equal(a[0], b[0])
        np.testing.assert_array_equal(a[2], b[2])
        self.assertEqual(float(a[0][18,32]), 500)
        self.assertEqual(int(a[2][18,32]), 8)

    def test_near_clip_preserves_plane_and_normal(self):
        positions = np.array([[-1.,0,10], [1.,0,30], [0.,2,30]])
        normals = np.tile([0.,0.,-1.], (3,1))
        triangles = list(clip_triangle(positions, normals))
        self.assertEqual(len(triangles), 2)
        for p, n in triangles:
            self.assertTrue(np.all(p[:,2] >= 20))
            np.testing.assert_allclose(n, normals)

    def test_bad_normal_indices_fail(self):
        row = self.row()
        row["faceNormalIndices"][0][0] = 9
        with self.assertRaisesRegex(ValueError, "normal index"):
            rasterize([row], self.camera(), 64, 36)

    def test_oblique_normal_changes_only_z(self):
        row = self.row()
        row["viewNormals"] = [[2, 3, 6]]
        depth, normals, _, _ = rasterize([row], self.camera(), 64, 36)
        expected = np.tile(np.array([2,3,-6])/7, ((depth > 0).sum(),1))
        np.testing.assert_allclose(normals[depth > 0], expected, atol=1e-7)

    def test_zero_source_normal_fails(self):
        row = self.row()
        row["viewNormals"] = [[0,0,0]]
        with self.assertRaisesRegex(ValueError, "zero-length"):
            rasterize([row], self.camera(), 64, 36)

    def test_unknown_or_nbt_normal_provenance_fails(self):
        for marker in (False, None):
            row = self.row()
            row["indexedNormalsAreFinal"] = marker
            with self.assertRaisesRegex(ValueError, "not verified final"):
                rasterize([row], self.camera(), 64, 36)

    def test_cancelling_normals_do_not_silently_drop_depth(self):
        row = self.row()
        row["vertices"] = [[-200,0,-1000], [200,0,-1000], [0,200,-1000]]
        row["viewNormals"] = [[-1,0,0], [1,0,0], [0,1,0]]
        row["faces"] = [[2,0,1,2]]
        row["faceNormalIndices"] = [[0,1,2]]
        with self.assertRaisesRegex(ValueError, "degenerate interpolated"):
            rasterize([row], self.camera(), 63, 37)

    def test_perspective_varying_normals_use_reciprocal_depth(self):
        camera = self.camera()
        screen = [(13,27,1000), (49,27,2000), (31,0,3000)]
        row = self.row()
        row["vertices"] = [[((x+.5)/63*2-1)*z*camera.tan_y*camera.aspect,
                            (1-(y+.5)/37*2)*z*camera.tan_y, -z]
                           for x,y,z in screen]
        row["viewNormals"] = [[1,0,0], [0,1,0], [0,0,1]]
        row["faces"] = [[2,0,1,2]]
        row["faceNormalIndices"] = [[0,1,2]]
        depth, normals, _, _ = rasterize([row], camera, 63, 37)
        reciprocal = np.array([1/1000, 1/2000, 1/3000])
        expected = reciprocal * [1,1,-1]
        expected /= np.linalg.norm(expected)
        self.assertAlmostEqual(float(depth[18,31]), 3/reciprocal.sum(), places=3)
        np.testing.assert_allclose(normals[18,31], expected, atol=1e-7)

    def test_far_clip_interpolates_normal_attributes(self):
        positions = np.array([[-1.,0,9990], [1.,0,10010], [0.,2,9990]])
        normals = np.array([[0.,0.,-1.], [1.,0.,0.], [0.,1.,0.]])
        triangles = list(clip_triangle(positions, normals))
        self.assertEqual(len(triangles), 2)
        boundary = []
        for p, n in triangles:
            self.assertTrue(np.all(p[:,2] <= 10000))
            boundary.extend(n[p[:,2] == 10000].tolist())
        self.assertIn([.5,0.,-.5], boundary)
        self.assertIn([.5,.5,0.], boundary)


if __name__ == "__main__":
    unittest.main()
