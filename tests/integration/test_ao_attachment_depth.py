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

    def test_water_is_removed_as_an_occluder_before_decal_cleanup(self):
        # A supplied final-visible-water mask is the contract of this fixture.
        # Raster mask generation/visibility is tested separately. Water crosses
        # a shoreline and one shoe; the foreground actor must survive the mask.
        w,h=257,129
        ground,_=plane(w,h)
        feet,contact=feet_scene(w,h)
        raised,decal_shape=plane(w,h,lift=18.)
        y,x=np.indices((h,w))
        actor=feet<ground-.001
        water_geometry=(x>w*.51+7*np.sin(y*.17))&(y>h*.12)
        visible_water=water_geometry&~actor
        decal_shape&=(x%7!=0)&(y%11!=0)
        after=np.where(decal_shape,raised,ground)
        # Discontinuous wave depths deliberately generate false geometry unless
        # rejected before AO. Some water equals the captured decal depth, so
        # substituting the underlying ground before the mask is insufficient.
        waves=ground-8.-12.*((x//11+y//9)%2)
        waves=np.where((x%5)==0,after,waves)
        coverage=np.zeros((h,w,4),dtype=np.float32)
        coverage[...,3]=visible_water/255.
        foliage=(x%13<5)&~visible_water
        coverage[foliage,:3]=[.08,.2,.04]
        scene=np.ones((h,w,4));scene[...,:3]=[.25,.625,.875];scene[...,3]=.375
        oracle_before=np.where(visible_water,0.,ground)
        oracle_after=np.where(visible_water,0.,after)
        self.assertTrue((water_geometry&actor).any())
        self.assertTrue((visible_water&decal_shape).any())
        contact_min=1.
        cases=0
        for renderer in (self.reference,*self.paths):
            decals=not renderer.attachment_depth or renderer.attachment_decals
            final=np.where(actor,feet,np.where(water_geometry,waves,after if decals else ground))
            oracle=np.where(visible_water,0.,final)
            for reverse in (False,True):
                for scale in (1.,.5):
                    args=dict(reversed_z=reverse,depth_range=(.1,.8),ao_scale=scale,
                              foliage=coverage,scene=scene)
                    # The oracle explicitly contains no water geometry. It
                    # does not depend on the new shader's masking branch.
                    oracle_coverage=coverage.copy();oracle_coverage[...,3]=0
                    expected=renderer.render(oracle,
                        decal_before=oracle_before if decals else None,
                        decal_after=oracle_after if decals else None,
                        **dict(args,foliage=oracle_coverage))
                    expected_depth=renderer.last_clean_depth.copy()
                    actual=renderer.render(final,
                        decal_before=ground if decals else None,
                        decal_after=after if decals else None,**args)
                    np.testing.assert_array_equal(renderer.last_clean_depth[visible_water],0.)
                    np.testing.assert_array_equal(renderer.last_clean_depth,expected_depth)
                    self.assertTrue((renderer.last_clean_depth[actor]>0).all())
                    # Raw AO, filtered AO and composed color must match the
                    # missing-water oracle, including the neighbouring shore.
                    for a,b in zip(actual,expected):
                        np.testing.assert_array_equal(a,b)
                    np.testing.assert_array_equal(actual[-1][visible_water],scene[visible_water])
                    np.testing.assert_array_equal(actual[-1][...,3],scene[...,3])
                    dry_contact=contact&~visible_water
                    darkening=float(np.quantile(1-actual[-1][dry_contact,0]/scene[dry_contact,0],.9))
                    contact_min=min(contact_min,darkening)
                    self.assertGreater(darkening,.025,
                        f'attachment={renderer.attachment_depth}, samples={renderer.depth_samples}, '
                        f'decals={decals}, reverse={reverse}, scale={scale}')
                    cases+=1
        print(f'Water occluder oracle: {cases} cases; clean-depth/raw/filter/composite max error 0; '
              f'minimum dry contact p90 darkening {contact_min:.6f}',flush=True)

    def test_rgb_foliage_retains_depth_and_empty_one_pixel_mask_is_neutral(self):
        z,_=feet_scene(129,65)
        h,w=z.shape
        full_empty=np.zeros((h,w,4),dtype=np.float32)
        single_empty=np.zeros((1,1,4),dtype=np.float32)
        rgb=full_empty.copy();rgb[...,:3]=[.125,.25,.0625]
        for renderer in (self.reference,*self.paths):
            args=dict(decal_before=z if not renderer.attachment_depth or renderer.attachment_decals else None,
                      decal_after=z if not renderer.attachment_depth or renderer.attachment_decals else None)
            baseline=renderer.render(z,foliage=full_empty,**args)
            baseline_depth=renderer.last_clean_depth.copy()
            empty=renderer.render(z,foliage=single_empty,**args)
            np.testing.assert_array_equal(renderer.last_clean_depth,baseline_depth)
            for actual,expected in zip(empty,baseline):
                np.testing.assert_array_equal(actual,expected)
            foliage=renderer.render(z,foliage=rgb,**args)
            np.testing.assert_array_equal(renderer.last_clean_depth,baseline_depth)
            for actual,expected in zip(foliage[:2],baseline[:2]):
                np.testing.assert_array_equal(actual,expected)
            self.assertTrue((renderer.last_clean_depth>0).all())

    def test_one_pixel_water_mask_excludes_all_geometry(self):
        z,_=feet_scene(129,65)
        mask=np.array([[[.125,.25,.0625,1/255.]]],dtype=np.float32)
        for renderer in (self.reference,*self.paths):
            args=dict(decal_before=z if not renderer.attachment_depth or renderer.attachment_decals else None,
                      decal_after=z if not renderer.attachment_depth or renderer.attachment_decals else None)
            raw,filtered,output=renderer.render(z,foliage=mask,**args)
            np.testing.assert_array_equal(renderer.last_clean_depth,0.)
            np.testing.assert_array_equal(raw[...,0],1.)
            np.testing.assert_array_equal(filtered[...,0],1.)
            np.testing.assert_array_equal(output,1.)


if __name__=='__main__':unittest.main(verbosity=2)
