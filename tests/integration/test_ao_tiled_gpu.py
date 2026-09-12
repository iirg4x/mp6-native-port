"""Run the real fused compute denoiser through the complete AO quality suite."""
import unittest
from test_ao_gpu import ShaderQuality, Renderer, load_shader_source, plane, feet_scene, np


class TiledShaderQuality(ShaderQuality):
    tiled = True


class TiledEquivalence(unittest.TestCase):
    def test_reference_kernel_raw_prepared_and_compatibility_paths(self):
        for compatibility in (False,True):
            for prepared in (False,True):
                source=load_shader_source(compatibility=compatibility,prepared_depth=prepared)
                reference=Renderer(mobile=True,shader_source=source,prepared_depth=prepared)
                tiled=Renderer(mobile=True,shader_source=source,prepared_depth=prepared,tiled=True)
                for width,height in ((1,1),(7,5),(17,9),(127,65),(511,257)):
                    ground,_=plane(width,height)
                    depth,_=feet_scene(width,height)
                    y,x=np.indices((height,width))
                    depth=np.minimum(depth,ground-35*((x//16+y//16)%2))
                    depth[:height//5,:width//3]=0
                    for reverse in (False,True):
                        for scale in (1.,.5):
                            args=dict(reversed_z=reverse,depth_range=(.1,.8),ao_scale=scale,
                                      viewport=(.03,.05,.9,.9),scissor=(.15,.1,.7,.8))
                            if prepared: args.update(decal_before=depth,decal_after=depth)
                            expected=reference.render(depth,**args)
                            actual=tiled.render(depth,**args)
                            np.testing.assert_array_equal(actual[0],expected[0])
                            np.testing.assert_array_equal(actual[1][...,1:],expected[1][...,1:])
                            for a,b in zip(actual[1:],expected[1:]):
                                self.assertTrue(np.isfinite(a).all())
                                # Intermediate binary16 rounding may differ from
                                # the render attachment's allowed rounding mode.
                                self.assertLessEqual(float(np.abs(a-b).max()),1./1024)
                            del actual,expected
                reference.device.destroy()
                tiled.device.destroy()


if __name__=='__main__':
    # Imported reference tests are not rerun by this executable.
    suite=unittest.TestSuite([unittest.defaultTestLoader.loadTestsFromTestCase(c)
                             for c in (TiledShaderQuality,TiledEquivalence)])
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
