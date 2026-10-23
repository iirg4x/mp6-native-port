"""Analytic CPU contracts for native leaf capture registration."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent / 'integration'))
try:
    from analyze_ao_leaf_motion import Camera, bilinear, motion_stats, np
except ModuleNotFoundError as error:
    if error.name != 'numpy':
        raise
    np = None

RECORD = dict(fov=45., aspect=1.59000015, viewport=[0,0,848,480], viewMatrix=[
    1,0,0,336.464111, 0,.951034129,-.309086084,926.045654,
    0,.309086084,.951034129,-3140.55225])


@unittest.skipIf(np is None, 'optional NumPy unavailable')
class LeafProjection(unittest.TestCase):
    def test_independent_view_space_projection_uses_camera_aspect(self):
        camera = Camera(RECORD, (1280,720))
        matrix = np.asarray(RECORD['viewMatrix']).reshape(3,4)
        for view, expected in (([0,0,-1000], [639.5,359.5,1000]),
                               ([100,50,-1000], [736.675882651,316.044155877,1000])):
            world = np.linalg.solve(matrix[:,:3], np.asarray(view)-matrix[:,3])
            np.testing.assert_allclose(camera.project(world), expected, atol=1e-9, rtol=0)

    def test_plane_roundtrip_even_and_odd_output_sizes(self):
        for size in ((1280,720), (853,479)):
            with self.subTest(size=size):
                camera = Camera(RECORD, size)
                u = np.array([0., (size[0]-1)/2, size[0]-1.])
                v = np.array([size[1]*.65, size[1]*.8, size[1]-1.])
                world = camera.world_on_plane(u, v, 5.132045198)
                x, y, depth = camera.project(world)
                np.testing.assert_allclose(world[:,1], 5.132045198, atol=1e-10)
                np.testing.assert_allclose(x, u, atol=1e-9)
                np.testing.assert_allclose(y, v, atol=1e-9)
                self.assertTrue(np.all(depth > 0))

    def test_lateral_camera_translation_moves_fixed_point_left(self):
        camera = Camera(RECORD, (1280,720))
        matrix = np.asarray(RECORD['viewMatrix']).reshape(3,4)
        point = np.linalg.solve(matrix[:,:3], np.array([0,0,-1000])-matrix[:,3])
        original = np.array(camera.project(point))
        shifted = camera.shifted_x(4.)
        difference = np.array(shifted.project(point))-original
        expected_x = -4.*1280/(2*np.tan(np.pi/8)*RECORD['aspect']*1000.)
        np.testing.assert_allclose(difference, [expected_x,0,0], atol=1e-10)
        center0 = -np.linalg.solve(matrix[:,:3], matrix[:,3])
        center1 = -np.linalg.solve(shifted.matrix[:,:3], shifted.matrix[:,3])
        np.testing.assert_allclose(center1-center0, [4,0,0], atol=1e-10)

    def test_bilinear_sampling_does_not_move_half_pixel_offset(self):
        image = np.repeat(np.arange(12).reshape(3,4,1), 3, axis=2)
        sampled, valid = bilinear(image, np.array([1.,1.5]), np.array([1.,1.5]))
        np.testing.assert_allclose(sampled[:,0], [5.,7.5])
        self.assertTrue(valid.all())

    def test_common_mask_and_signed_changes_are_preserved(self):
        values = [np.array([[2.,100.],[-1.,50.]]), np.array([[4.,-50.],[-3.,90.]])]
        common = np.array([[True,False],[True,False]])
        measured = motion_stats(values, common)
        self.assertEqual(measured['mean'], .5)
        self.assertEqual(measured['std_mean'], 1.)
        self.assertEqual(measured['step_max'], 2.)

    def test_unrecorded_partial_viewport_is_rejected(self):
        with self.assertRaises(ValueError):
            Camera(dict(RECORD, viewport=[4,0,844,480]), (1280,720))


if __name__ == '__main__':
    unittest.main()
