import copy
import math
import unittest

from cs2_mvp.composition import compose_pair
from cs2_mvp.camera import compile_camera


class CompositionTests(unittest.TestCase):
    def setUp(self):
        self.spec = {'version': 1, 'first': {'min': [-16, -16, 0], 'max': [16, 16, 72]},
                     'second': {'min': [496, -16, 0], 'max': [528, 16, 72]},
                     'horizontal_fov': 90, 'aspect_ratio': 16 / 9, 'margin': .1}

    def test_both_complete_models_fit_and_camera_is_above_them(self):
        result = compose_pair(self.spec)
        report = result['composition']
        self.assertGreater(result['position'][2], 72 + 48)
        self.assertGreater(report['first_to_second_screen_height'], 1)
        first, second = report['subjects']
        self.assertLess(sum(first['min_uv'][0:1] + first['max_uv'][0:1]),
                        sum(second['min_uv'][0:1] + second['max_uv'][0:1]))
        # Independently reconstruct the projection from the exported pitch/yaw.
        pitch, yaw = map(math.radians, result['angles'])
        forward = (math.cos(pitch)*math.cos(yaw), math.cos(pitch)*math.sin(yaw), -math.sin(pitch))
        right = (math.sin(yaw), -math.cos(yaw), 0)
        up = (math.sin(pitch)*math.cos(yaw), math.sin(pitch)*math.sin(yaw), math.cos(pitch))
        from itertools import product
        for model in [self.spec['first'], self.spec['second']]:
            for corner in product(*zip(model['min'], model['max'])):
                q = [v-c for v,c in zip(corner, result['position'])]
                z = sum(a*b for a,b in zip(q,forward))
                u = .5 + sum(a*b for a,b in zip(q,right)) / (2*z)
                v = .5 - sum(a*b for a,b in zip(q,up)) * (16/9) / (2*z)
                self.assertGreaterEqual(z, 8)
                self.assertTrue(.1-1e-6 <= u <= .9+1e-6)
                self.assertTrue(.1-1e-6 <= v <= .9+1e-6)
        compiled = compile_camera(result, None, 1000, 1064, 30)
        self.assertEqual(compiled['frames'][0], compiled['frames'][-1])
        self.assertFalse(report['lens_applied'])

    def test_rotation_and_translation_preserve_composition(self):
        a = compose_pair(self.spec)
        changed = copy.deepcopy(self.spec)
        for model in [changed['first'], changed['second']]:
            low, high = model['min'], model['max']
            model['min'] = [-high[1]+1000, low[0]-800, low[2]+200]
            model['max'] = [-low[1]+1000, high[0]-800, high[2]+200]
        b = compose_pair(changed)
        expected = [-a['position'][1]+1000, a['position'][0]-800, a['position'][2]+200]
        for actual, wanted in zip(b['position'], expected):
            self.assertAlmostEqual(actual, wanted)
        self.assertAlmostEqual(b['composition']['first_to_second_screen_height'], a['composition']['first_to_second_screen_height'])

    def test_elevation_difference_and_portrait_fit(self):
        self.spec['second'] = {'min': [496, -16, -420], 'max': [528, 16, -348]}
        self.spec['aspect_ratio'] = 9/16
        result = compose_pair(self.spec)
        for subject in result['composition']['subjects']:
            self.assertTrue(all(v >= .1-1e-6 for v in subject['min_uv']))
            self.assertTrue(all(v <= .9+1e-6 for v in subject['max_uv']))

    def test_invalid_geometry_and_infeasible_distance_fail(self):
        for key, value in [('horizontal_fov', True), ('aspect_ratio', 0), ('elevation', 0),
                           ('side_angle', 0), ('margin', .5), ('max_distance', 1)]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                compose_pair({**self.spec, key: value})
        for invalid in [{'min': [0,0,0], 'max': [0,16,72]},
                        {'min': [0,0,0], 'max': [math.nan,16,72]}, self.spec['first']]:
            with self.assertRaises(ValueError):
                compose_pair({**self.spec, 'second': invalid})


if __name__ == '__main__':
    unittest.main()
