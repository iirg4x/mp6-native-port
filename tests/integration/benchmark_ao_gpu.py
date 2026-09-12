"""GPU timestamp timings of production AO passes; excludes game, copies and present."""
import json
import statistics
from test_ao_gpu import Renderer, feet_scene, plane, np

renderer = Renderer(timing=True)
print(json.dumps(dict(renderer.adapter.info)), flush=True)
for width, height in ((1280, 720), (1920, 1080), (3840, 2160)):
    ground, _ = plane(width, height)
    raised, mask = plane(width, height, lift=3.)
    after = np.where(mask, raised, ground)
    feet, _ = feet_scene(width, height)
    final = np.where(feet < ground-.001, feet, after)
    samples = []
    for i in range(6):
        renderer.render(final, decal_before=ground, decal_after=after, ping_pong=True, repeat=256)
        if i >= 1:
            samples.append(renderer.last_timings_ms)
    names = ['gtao', 'blur_x', 'blur_y', 'composite', 'decal_depth']
    medians = [statistics.median(row[i] for row in samples) for i in range(5)]
    print(json.dumps(dict(resolution=[width, height], median_gpu_ms=dict(zip(names, medians)),
                          total_gpu_ms=sum(medians))), flush=True)
