"""Bound the analytic open-arc rounding separately from exact AO fast paths."""
import math
import unittest
from test_ao_gpu import Renderer, plane, feet_scene, np, load_shader_source


def general_open_integral(source):
    """Use the original endpoint quadrature; leave every other operation alone."""
    call = 'let openArc = open_horizon_arc(n, low);'
    if call not in source:
        return source
    assert source.count(call) == 1
    return source.replace(call, '''let openAngles = vec2f(-acos(clamp(low.y, -1.0, 1.0)), acos(clamp(low.x, -1.0, 1.0)));
        let openArc = horizon_arc(openAngles.x, n) + horizon_arc(openAngles.y, n);''')


class OpenHorizonIntegral(unittest.TestCase):
    def test_normal_coverage_and_sub_display_rounding(self):
        for prepared in (False, True):
            source = load_shader_source(compatibility=False, prepared_depth=prepared)
            self.assertIn('open_horizon_arc(n, low)', source)
            self.assertIn('if (all(horizon == low))', source)
            before, after = [Renderer(mobile=True, prepared_depth=prepared, shader_source=s)
                             for s in (general_open_integral(source), source)]
            for w, h in ((511, 257), (1920, 1080)):
                ground, _ = plane(w, h)
                feet, _ = feet_scene(w, h)
                y, x = np.indices((h, w))
                dense = ground - 35 * ((x // 16 + y // 16) % 2)
                for z in (ground, feet, dense, np.zeros((h, w))):
                    for reverse in (False, True):
                        args = dict(reversed_z=reverse)
                        if prepared:
                            args.update(decal_before=z, decal_after=z)
                        expected, actual = before.render(z, **args), after.render(z, **args)
                        for stage, (a, b) in enumerate(zip(expected, actual)):
                            self.assertTrue(np.isfinite(b).all())
                            np.testing.assert_array_equal(a[..., 1:] if stage < 2 else a[..., 3],
                                                          b[..., 1:] if stage < 2 else b[..., 3])
                            self.assertLessEqual(float(np.abs(a - b).max()), 1 / 2048)
                            if z is ground or not z.any():
                                np.testing.assert_array_equal(a, b)

    def test_open_arc_identity_in_viewer_hemisphere(self):
        def arc(h, n):
            return (math.cos(n) + 2 * h * math.sin(n) - math.cos(2 * h - n)) * .25
        for n in np.linspace(-math.pi / 2, math.pi / 2, 10001):
            low = math.cos(n + math.pi / 2), math.cos(n - math.pi / 2)
            endpoints = -math.acos(low[1]), math.acos(low[0])
            expected = sum(arc(h, n) for h in endpoints)
            self.assertAlmostEqual(expected, math.cos(n) + n * math.sin(n), places=12)


if __name__ == '__main__':
    unittest.main(verbosity=2)
