import math
import unittest

from cs2_mvp.camera import SubjectTrack, compile_camera
from cs2_mvp.pose_track import FovKeys, PoseKeys, from_angles, quaternion, slerp, to_angles


class PoseKeysTests(unittest.TestCase):
    def test_known_quaternion_and_euler_roundtrip(self):
        q = [0,0,math.sqrt(.5),math.sqrt(.5)]
        actual = to_angles(q)
        self.assertAlmostEqual(actual[1],90)
        for value in [[25,160,12],[-40,-140,-30],[0,0,0]]:
            for a,b in zip(to_angles(from_angles(value)),value):
                self.assertAlmostEqual(a,b)
        for value in [[90,45,30],[-90,70,15]]:
            q = from_angles(value)
            restored = from_angles(to_angles(q))
            self.assertAlmostEqual(abs(sum(a*b for a,b in zip(q,restored))),1)
        self.assertEqual(quaternion([0,0,0,2]), (0,0,0,1))

    def test_short_arc_and_quaternion_sign_equivalence(self):
        a,b = from_angles([0,179,0]),from_angles([0,-179,0])
        self.assertAlmostEqual(abs(to_angles(slerp(a,b,.5))[1]),180)
        c = slerp(a,tuple(-v for v in a),.5)
        self.assertAlmostEqual(abs(sum(x*y for x,y in zip(a,c))),1)

    def test_fixed_world_position_pan_does_not_consult_actor(self):
        spec = {'version':1,'type':'keyframes','position':[20,30,220], 'rotation_easing':'linear',
                'rotation_keys':[{'time':0,'angles':[20,60,0]},{'time':2,'angles':[20,0,10]}]}
        a = compile_camera(spec,None,1000,1128,60)
        actor = SubjectTrack([1000,1128],[(1,2,3),(10000,20000,30000)],'ignored','1')
        self.assertEqual(a,compile_camera(spec,None,1000,1128,60,subject=actor))
        self.assertTrue(all(f['position']==[20,30,220] for f in a['frames']))
        self.assertNotEqual(a['frames'][0]['angles'],a['frames'][-1]['angles'])
        self.assertEqual(len(a['frames'][0]['angles']),3)
        self.assertIsNone(a['summary']['tracked_player'])

    def test_two_position_keys_ease_once_and_rotation_has_its_own_clock(self):
        spec={'position_easing':'smootherstep','position_keys':[{'time':0,'position':[0,0,200]},
              {'time':2,'position':[200,0,200]}], 'rotation_easing':'linear',
              'rotation_keys':[{'time':0,'quaternion':[0,0,0,1]},
                               {'time':1,'angles':[0,90,0]},{'time':2,'angles':[0,90,0]}]}
        track=PoseKeys(spec,2)
        self.assertLess(track.pose(.1)[0][0],1)
        self.assertAlmostEqual(track.pose(1)[0][0],100)
        self.assertAlmostEqual(track.pose(2)[0][0],200)
        self.assertAlmostEqual(track.pose(.5)[1][1],45)
        self.assertAlmostEqual(track.pose(1.5)[1][1],90)

    def test_invalid_keys_fail_instead_of_guessing_or_attaching_to_player(self):
        base={'position':[0,0,200], 'rotation_keys':[{'time':0,'angles':[0,0,0]}]}
        for update in [{'player':'CT'}, {'position_keys':[{'time':0,'position':[1,2,3]}]},
                       {'rotation_keys':[{'time':0,'quaternion':[0,0,0,0]}]},
                       {'rotation_keys':[{'time':1,'angles':[0,0,0]}]},
                       {'rotation_keys':[{'time':0,'angles':[0,0,0]},{'time':0,'angles':[0,90,0]}]},
                       {'rotation_keys':[{'time':0,'angles':[0,0,0]},{'time':1,'angles':[0,90,0]}]}]:
            with self.subTest(update=update), self.assertRaises(ValueError):
                PoseKeys({**base,**update},2)

    def test_world_interest_point_authors_two_rotations_without_actor_tracking(self):
        from cs2_mvp.camera import look_angles
        spec={'version':1,'type':'keyframes', 'position_keys':[
            {'time':0,'position':[-280,-1700,230]}, {'time':2,'position':[-320,-1950,260]}],
            'point_of_interest':[120,-1660,32]}
        track=PoseKeys(spec,2)
        self.assertEqual(len(track.positions),2)
        self.assertEqual(len(track.rotations),2)
        for time in [0,2]:
            position, angles=track.pose(time)
            pitch,yaw=look_angles(position,spec['point_of_interest'])
            self.assertAlmostEqual(angles[0],pitch)
            self.assertAlmostEqual(angles[1],yaw)
        expected=compile_camera(spec,None,1000,1128,120)
        actor=SubjectTrack([1000,1128],[(0,0,0),(5000,9000,-3000)],'ignored','1')
        self.assertEqual(expected,compile_camera(spec,None,1000,1128,120,subject=actor))
        self.assertIsNone(expected['summary']['tracked_player'])
        with self.assertRaises(ValueError):
            PoseKeys({**spec,'rotation_keys':[{'time':0,'angles':[0,0,0]}]},2)

    def test_three_stages_hold_final_world_pose_as_actor_falls(self):
        spec={'version':1,'type':'keyframes','hold_last':True,'position_keys':[
            {'time':0,'position':[0,0,200]}, {'time':1,'position':[10,0,220]},
            {'time':2,'position':[30,0,200]}], 'rotation_keys':[
            {'time':0,'angles':[10,20,0]}, {'time':1,'angles':[20,30,0]},
            {'time':2,'angles':[40,40,0]}]}
        track=PoseKeys(spec,4)
        self.assertEqual(track.pose(2),track.pose(3.9))
        actor=SubjectTrack([1000,1256],[(0,0,0),(0,0,-1000)],'falling actor','1')
        first=compile_camera(spec,None,1000,1256,120)
        self.assertEqual(first,compile_camera(spec,None,1000,1256,120,subject=actor))
        self.assertEqual(first['frames'][240],first['frames'][-1])
        with self.assertRaises(ValueError):
            PoseKeys({**spec,'hold_last':False},4)

    def test_close_lens_holds_then_eases_out_without_changing_the_world_pose(self):
        spec={'version':1,'type':'keyframes','position':[0,0,200],
              'rotation_keys':[{'time':0,'angles':[20,30,0]}],'hold_last':True,
              'fov_keys':[{'time':0,'fov':35},{'time':3,'fov':35},{'time':5,'fov':45}]}
        lens=FovKeys(spec,6)
        for t in [0,1,2.9,3]: self.assertEqual(lens.sample(t),35)
        self.assertAlmostEqual(lens.sample(4),40)
        self.assertEqual(lens.sample(6),45)
        result=compile_camera(spec,None,1000,1384,120)
        self.assertEqual(result['summary']['fov_animation'],{'start':35,'end':45,'minimum':35,'maximum':45})
        self.assertTrue(all(f['position']==[0,0,200] for f in result['frames']))
        self.assertTrue(all(f['angles']==result['frames'][0]['angles'] for f in result['frames']))
        for update in [{'fov':35},{'fov_easing':'invalid'},
                       {'fov_keys':[{'time':0,'fov':True}]},
                       {'fov_keys':[{'time':0,'fov':29}]},
                       {'fov_keys':[{'time':0,'fov':math.nan}]},
                       {'hold_last':False}]:
            with self.subTest(update=update), self.assertRaises(ValueError):
                FovKeys({**spec,**update},6)

    def test_curved_travel_has_continuous_velocity_at_uneven_key_times(self):
        spec = {'hold_last': True, 'position_interpolation': 'hermite',
                'position_keys': [{'time': 0, 'position': [0, 0, 200]},
                                  {'time': 1, 'position': [100, 20, 240]},
                                  {'time': 3, 'position': [200, -50, 260]},
                                  {'time': 5, 'position': [300, 0, 200]}],
                'rotation_keys': [{'time': 0, 'angles': [20, 30, 0]}]}
        track = PoseKeys(spec, 6)
        step = 1e-5
        for key in spec['position_keys']:
            t, expected = key['time'], key['position']
            self.assertEqual(track.pose(t)[0], tuple(expected))
            if t in (1, 3):
                before, at, after = (track.pose(v)[0] for v in [t-step, t, t+step])
                for a, b, c in zip(before, at, after):
                    self.assertAlmostEqual((b-a)/step, (c-b)/step, delta=.01)
        for endpoint, neighbor in [(0, step), (5, 5-step)]:
            self.assertLess(math.dist(track.pose(endpoint)[0], track.pose(neighbor)[0])/step, .01)
        self.assertEqual(track.pose(5), track.pose(6))
        self.assertNotEqual(track.pose(2)[0][1], -15)
        for update in [{'position_interpolation': 'unknown'}, {'position_easing': 'smoothstep'}]:
            with self.assertRaises(ValueError):
                PoseKeys({**spec, **update}, 6)

    def test_curved_travel_floor_check_includes_overshoot_between_keys(self):
        spec = {'version': 1, 'type': 'keyframes', 'position_interpolation': 'hermite',
                'position_keys': [{'time': 0, 'position': [0, 0, 200]},
                                  {'time': 1, 'position': [100, 0, 100]},
                                  {'time': 2, 'position': [200, 0, 100]},
                                  {'time': 3, 'position': [300, 0, 200]}],
                'rotation_keys': [{'time': 0, 'angles': [20, 30, 0]}],
                'floor_z': 0, 'clearance': 100}
        with self.assertRaisesRegex(ValueError, 'below floor clearance'):
            compile_camera(spec, None, 1000, 1192, 120)


if __name__ == '__main__':
    unittest.main()
