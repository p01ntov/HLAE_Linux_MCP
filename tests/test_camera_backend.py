import unittest
from pathlib import Path
import tempfile

from cs2_mvp.camera_backend import build_camera_sequence, build_static_camera_sequence, validate_motion_log, verify_cosmetics_capture_support
from cs2_mvp.csdm import build_actions
from cs2_mvp.capture_environment import build_environment_sequence


class CameraBackendTests(unittest.TestCase):
    def test_explicit_xray_scene_override_follows_clean_camera_preroll_default(self):
        base = build_actions(1000, 1064, "", "xray", 1000, camera_only=True)
        pose = {"position": [0, 0, 64], "angles": [0, 90, 0]}
        for camera in (build_camera_sequence(base, [pose] * 1000), build_static_camera_sequence(base, pose)):
            for enabled in (True, False):
                sequence = build_environment_sequence(camera, {"scene": {"xray_enabled": enabled}})
                setup = [(index, action) for index, action in enumerate(sequence["actions"]) if action["tick"] == 936]
                default = next(index for index, action in setup if action["cmd"] == "spec_show_xray 0")
                explicit = next(index for index, action in setup if action["cmd"].startswith("cs2_mvp_scene "))
                self.assertGreater(explicit, default)
                self.assertLess(explicit, next(index for index, action in enumerate(sequence["actions"])
                    if action["cmd"] == "startmovie xray"))
                self.assertFalse(any(action["cmd"].startswith("cs2_mvp_scene ") for action in camera["actions"]))

    def test_cosmetic_capture_requires_both_application_and_restoration_support(self):
        markers=[b"CS2_MVP_MOTION_ABI 3",b"CS2_MVP_ENVIRONMENT_ABI 1",b"CS2_MVP_COSMETICS_APPLIED",b"CS2_MVP_COSMETICS_RESTORED"]
        with tempfile.TemporaryDirectory() as directory:
            plugin=Path(directory)/"capture.so"
            plugin.write_bytes(b"\x7fELF\x02\x01"+b"\0".join(markers))
            self.assertEqual(verify_cosmetics_capture_support(plugin),plugin)
            for omitted in markers:
                plugin.write_bytes(b"\x7fELF\x02\x01"+b"\0".join(marker for marker in markers if marker!=omitted))
                with self.subTest(omitted=omitted),self.assertRaisesRegex(RuntimeError,"readback and restoration"):
                    verify_cosmetics_capture_support(plugin)

    def test_actorless_capture_and_internal_control_order(self):
        base = build_actions(1000, 1064, '', 'camera_test', 120, camera_only=True)
        frames = [{'position': [1, 2, 3], 'angles': [0, 90]}] * 120
        sequence = build_camera_sequence(base, frames)
        commands = [a['cmd'] for a in sequence['actions']]
        self.assertEqual(commands[commands.index('startmovie camera_test') - 1], 'camera_begin')
        self.assertEqual(commands[commands.index('endmovie') - 1], 'camera_end')
        self.assertFalse(any(a['cmd'] == 'camera_begin' for a in base['actions']))
        self.assertFalse(any(c.startswith('spec_player ') for c in commands))
        self.assertEqual(len(sequence['camera_frames']), 120)
        self.assertIn('spec_mode 4', commands)  # Console state is not proof of final view control.

    def test_stock_plugin_cannot_silently_pass(self):
        with self.assertRaises(RuntimeError):
            validate_motion_log('[1000] Executing: startmovie camera_test', 120)
        log = ('CS2_MVP_MOTION_ABI 3\nCS2_MVP_CAMERA_BEGIN expected=120 tick=1000\n'
               'CS2_MVP_CAMERA_STAGE FRAME_START_LINUX_MAIN_VIEW\n'
               'CS2_MVP_VIEW_HOOK linux_build=d5bacd2f5df4a3a68fa8bee8692337993548e14a\n'
               'CS2_MVP_CAMERA_END applied=120 expected=120 callbacks=120 overflow=0 tick=1064 reason=camera_end\n'
               'CS2_MVP_VIEW_END writes=120 fault=0 tick=1064')
        self.assertEqual(validate_motion_log(log, 120)['callbacks'], 120)
        with self.assertRaises(RuntimeError):
            validate_motion_log(log.replace('callbacks=120', 'callbacks=64'), 120)
        with self.assertRaises(RuntimeError):
            validate_motion_log(log.replace('writes=120', 'writes=64'), 120)
        with self.assertRaises(RuntimeError):
            validate_motion_log(log, 120, 105)
        confirmed = log + '\nCS2_MVP_FOV_END writes=120 value=105.000000'
        self.assertEqual(validate_motion_log(confirmed,120,105)['fov_override'],105)
        with self.assertRaises(RuntimeError):
            validate_motion_log(confirmed.replace('value=105.000000','value=90.000000'),120,105)
        profile={'start':35,'end':45,'minimum':35,'maximum':45}
        animated=log+'\nCS2_MVP_FOV_END writes=120 value=45.000000 min=35.000000 max=45.000000'
        self.assertEqual(validate_motion_log(animated,120,profile)['fov_override'],profile)
        with self.assertRaises(RuntimeError):
            validate_motion_log(animated.replace('min=35.000000','min=45.000000'),120,profile)

    def test_motion_hook_accepts_only_one_allowlisted_client_build(self):
        previous="d5bacd2f5df4a3a68fa8bee8692337993548e14a"
        current="f1e04e3616921a3bb25940eea9b4b6bfd7ccada5"
        updated="f00942ca6d0c486d11f988c53381c47a5be27143"
        latest="c6c5098eab52964d9d088279ca5cc991c76d1a7d"
        prefix=("CS2_MVP_MOTION_ABI 3\nCS2_MVP_CAMERA_BEGIN expected=120 tick=1000\n"
                "CS2_MVP_CAMERA_STAGE FRAME_START_LINUX_MAIN_VIEW\n")
        suffix=("CS2_MVP_CAMERA_END applied=120 expected=120 callbacks=120 overflow=0 tick=1064 reason=camera_end\n"
                "CS2_MVP_VIEW_END writes=120 fault=0 tick=1064\n")
        def hook(build):
            return "CS2_MVP_VIEW_HOOK linux_build="+build+"\n"
        for build in (previous,current,updated,latest):
            with self.subTest(build=build):
                self.assertEqual(validate_motion_log(prefix+hook(build)+suffix,120)["client_build_id"],build)
        for records in ("",hook("0"*40),hook("not-a-build-id"),hook(current)+hook(current),
                        hook(previous)+hook(current),hook(current)+hook(updated),hook(current)+hook("0"*40),hook(current+"-extra"),
                        hook(current)+hook("not-a-build-id"),hook(current)+hook(""),hook(latest)+hook(latest),
                        hook(updated)+hook(latest),hook(latest+"-extra"),hook(latest[:-1]+"0")):
            with self.subTest(records=records),self.assertRaisesRegex(RuntimeError,"Late Linux main-view"):
                validate_motion_log(prefix+records+suffix,120)

    def test_fov_survives_the_native_sidecar_boundary(self):
        base=build_actions(1000,1064,'','wide',120,camera_only=True)
        pose={'position':[0,0,200],'angles':[10,20,0],'fov':105}
        self.assertEqual(build_camera_sequence(base,[pose])['camera_frames'][0]['fov'],105)
        for value in [True, float('nan'),29,141]:
            with self.assertRaises(ValueError):
                build_camera_sequence(base,[{**pose,'fov':value}])

    def test_fixed_camera_is_applied_once_before_recording(self):
        base = build_actions(1000, 1064, '', 'fixed', 120, camera_only=True)
        sequence = build_static_camera_sequence(base, {'position': [100, 200, 300], 'angles': [-10, 20]})
        self.assertNotIn('camera_frames', sequence)
        commands = [a for a in sequence['actions'] if a['cmd'].startswith('spec_goto ')]
        self.assertEqual(len(commands), 1)
        self.assertLess(commands[0]['tick'], 1000)
        self.assertFalse(any(a['cmd'] in {'camera_begin', 'camera_end'} for a in sequence['actions']))


if __name__ == '__main__':
    unittest.main()
