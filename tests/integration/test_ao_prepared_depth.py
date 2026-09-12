"""Prepared R32 depth must retain the existing AO, coverage and surface normals."""
import unittest
from test_ao_gpu import Renderer, load_shader_source, plane, feet_scene, np


class PreparedDepth(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.paths=[]
        for compatibility in (False,True):
            cls.paths.append((Renderer(mobile=True,shader_source=load_shader_source(compatibility=compatibility)),
                              Renderer(mobile=True,prepared_depth=True,
                                       shader_source=load_shader_source(compatibility=compatibility,prepared_depth=True))))

    def compare(self,depth,**args):
        for raw,prepared in self.paths:
            expected=raw.render(depth,**args)
            actual=prepared.render(depth,**args)
            for a,b in zip(actual,expected):
                self.assertTrue(np.isfinite(a).all())
                np.testing.assert_array_equal(a,b)

    def test_native_reduced_and_odd_resolution_contacts_and_cutouts(self):
        for w,h in ((511,257),(1920,1080),(2401,1081)):
            ground,_=plane(w,h)
            feet,_=feet_scene(w,h)
            raised,mask=plane(w,h,lift=18.)
            y,x=np.indices((h,w)); mask &= (x%7!=0)&(y%11!=0)
            decal=np.where(mask,raised,ground)
            final=np.where(feet<ground-.001,feet,decal)
            self.compare(final,decal_before=ground,decal_after=decal)

    def test_reversed_depth_ranges_scissors_and_empty_pixels(self):
        w,h=511,257
        ground,_=plane(w,h)
        y,x=np.indices((h,w))
        depth=ground-35*((x//16+y//16)%2)
        depth[:70,:]=0
        for reverse in (False,True):
            for scale in (1.,.5):
                self.compare(depth,decal_before=depth,decal_after=depth,reversed_z=reverse,
                             ao_scale=scale,depth_range=(.1,.8),
                             viewport=(.03,.05,.9,.9),scissor=(.15,.1,.7,.8))

    def test_far_slopes_preserve_full_depth_precision(self):
        for distance in (150.,2400.,6000.):
            ground,_=plane(511,257,distance=distance)
            self.compare(ground,decal_before=ground,decal_after=ground)
        empty=np.zeros((129,257))
        self.compare(empty,decal_before=empty,decal_after=empty)

    def test_colored_foreground_water_and_alpha_are_unchanged(self):
        h,w=128,256
        depth=np.full((h,w),400.)
        y,x=np.indices((h,w))
        coverage=np.zeros((h,w,4));coverage[...,:3]=[.1,.3,.05]
        coverage[...,3]=((x>80)&(x<190)&(y%7!=0)&(x%11!=0))/255.
        scene=np.ones((h,w,4));scene[...,:3]=[.25,.625,.875];scene[...,3]=.375
        for scale in (1.,.5):
            signal=np.zeros((int(h*scale),int(w*scale),4));signal[...,0]=.65;signal[...,3]=1
            self.compare(depth,decal_before=depth,decal_after=depth,ao_scale=scale,
                         override_filtered=signal,foliage=coverage,scene=scene)

    def test_preparation_does_not_enable_withdrawn_mali_fast_paths(self):
        source=load_shader_source(compatibility=True,prepared_depth=True)
        self.assertNotIn('var samples: array<vec4f,7>',source)
        self.assertNotIn('if (elevation <= 2.0)',source)
        # Only depth conversion changes: no horizon/filter/composite rewrite.
        original=load_shader_source(compatibility=True)
        self.assertEqual(source[source.index('fn position('):],original[original.index('fn position('):])


if __name__=='__main__':unittest.main(verbosity=2)
