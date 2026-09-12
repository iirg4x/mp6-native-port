"""Exact direct-attachment depth preparation against the old snapshot path."""
import unittest
from test_ao_gpu import Renderer, load_shader_source, plane, feet_scene, np


class AttachmentDepth(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reference=Renderer(mobile=True,prepared_depth=True)
        cls.paths=[Renderer(mobile=True,prepared_depth=True,attachment_depth=True,
                           depth_samples=samples,attachment_decals=decals)
                   for samples in (1,4) for decals in (False,True)]

    def compare(self,depth,before=None,after=None,**args):
        for direct in self.paths:
            decals=direct.attachment_decals
            expected=self.reference.render(depth,decal_before=before if decals and before is not None else depth,
                decal_after=after if decals and after is not None else depth,**args)
            expected_depth=self.reference.last_clean_depth.copy()
            actual=direct.render(depth,decal_before=before if decals else None,
                                 decal_after=after if decals else None,**args)
            np.testing.assert_array_equal(direct.last_clean_depth,expected_depth)
            for a,b in zip(actual,expected):
                np.testing.assert_array_equal(a,b)

    def test_single_and_multisample_sample_zero_contacts_decals(self):
        for w,h in ((129,65),(511,257),(1920,1080)):
            ground,_=plane(w,h)
            feet,_=feet_scene(w,h)
            raised,mask=plane(w,h,lift=18.)
            y,x=np.indices((h,w));mask&=(x%7!=0)&(y%11!=0)
            decal=np.where(mask,raised,ground)
            final=np.where(feet<ground-.001,feet,decal)
            self.compare(final,ground,decal)

    def test_no_decals_empty_scissor_and_depth_ranges(self):
        ground,_=plane(257,129)
        ground[:20]=0
        for reverse in (False,True):
            self.compare(ground,reversed_z=reverse,depth_range=(.1,.8),ao_scale=.5,
                viewport=(.03,.05,.9,.9),scissor=(.15,.1,.7,.8))
        self.compare(np.zeros((65,129)))

    def test_foliage_water_and_alpha(self):
        h,w=65,129
        y,x=np.indices((h,w))
        coverage=np.zeros((h,w,4));coverage[...,:3]=[.1,.3,.05]
        coverage[...,3]=((x>30)&(x<90)&(y%7!=0)&(x%11!=0))/255.
        scene=np.ones((h,w,4));scene[...,:3]=[.25,.625,.875];scene[...,3]=.375
        signal=np.zeros((h,w,4));signal[...,0]=.65;signal[...,3]=1
        self.compare(np.full((h,w),400.),ao_scale=1.,override_filtered=signal,foliage=coverage,scene=scene)


if __name__=='__main__':unittest.main(verbosity=2)
