import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from cs2_mvp.render import command_ticks, config_backup, owned_raw_files, session_processes, stop_process


class PipelineTests(unittest.TestCase):
    def test_capture_ticks_ignore_json_echo_and_read_actual_execution(self):
        log = '\n'.join([
            'CSDM: {"tick": 12000, "cmd": "startmovie cs2mvp_abc"}',
            'CSDM: [12001] Executing: startmovie cs2mvp_abc',
            'CSDM: [12001] Executed: startmovie cs2mvp_abc',
            'CSDM: [12640] Executing: endmovie',
        ])
        self.assertEqual(command_ticks(log, 'cs2mvp_abc'), {'start': 12001, 'end': 12640})
        self.assertEqual(command_ticks(log.replace('CSDM: ', ''), 'cs2mvp_abc'), {'start': 12001, 'end': 12640})

    def test_raw_discovery_includes_audio_outside_plugin_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            game = Path(temporary)
            for relative in ['csgo/csdm/movie/test/job00000000.tga', 'csgo/movie/test/job.wav',
                             'csgo/csdm/movie/test/unrelated00000000.tga']:
                path = game / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'fixture')
            self.assertEqual({p.name for p in owned_raw_files(game, 'job')}, {'job00000000.tga', 'job.wav'})

    def test_configuration_restored_on_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            profile = root / 'steam/userdata/1/730/local/cfg'
            profile.mkdir(parents=True)
            (profile / 'cs2_video.txt').write_bytes(b'original\r\n')
            run = root / 'run'
            run.mkdir()
            with patch('cs2_mvp.render.steam_roots', return_value=[root / 'steam']), patch('cs2_mvp.render.process_ids', return_value=[]):
                with self.assertRaisesRegex(RuntimeError, 'fake failure'):
                    with config_backup(run):
                        (profile / 'cs2_video.txt').write_bytes(b'modified')
                        (profile / 'new.vcfg').write_bytes(b'new')
                        raise RuntimeError('fake failure')
            self.assertEqual((profile / 'cs2_video.txt').read_bytes(), b'original\r\n')
            self.assertFalse((profile / 'new.vcfg').exists())
            self.assertEqual(len(json.loads((run / 'config-backup.json').read_text())), 1)

    def test_new_config_is_saved_before_removing_a_previously_absent_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            account = root / 'steam/userdata/1'
            account.mkdir(parents=True)
            profile = account / '730/local/cfg'
            run = root / 'run'
            run.mkdir()
            with patch('cs2_mvp.render.steam_roots', return_value=[root / 'steam']), patch('cs2_mvp.render.process_ids', return_value=[]):
                with config_backup(run):
                    profile.mkdir(parents=True)
                    (profile / 'new.vcfg').write_bytes(b'created by game')
            self.assertFalse(profile.exists())
            self.assertEqual((run / 'changed-config/0/new.vcfg').read_bytes(), b'created by game')

    def test_runtime_descendant_stops_even_after_wrapper_exits(self):
        script = ('import subprocess,sys; '
                  'p=subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"],'
                  'stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); print(p.pid,flush=True)')
        process = subprocess.Popen([sys.executable, '-c', script], start_new_session=True,
                                   stdout=subprocess.PIPE, text=True)
        try:
            child = int(process.stdout.readline())
            process.wait(timeout=5)
            self.assertIn(child, session_processes(process.pid))
            stop_process(process)
            self.assertEqual(session_processes(process.pid), [])
        finally:
            stop_process(process)
            process.stdout.close()

    def test_owned_game_stops_after_escaping_the_runtime_session(self):
        with tempfile.TemporaryDirectory() as temporary, patch('cs2_mvp.render.ROOT', Path(temporary)):
            demo=Path(temporary)/'.runs/owned/input.dem'
            demo.parent.mkdir(parents=True)
            demo.touch()
            child_script=('import os,ctypes,time; os.setsid(); '
                          'ctypes.CDLL(None).prctl(15,b"cs2",0,0,0); print(os.getpid(),flush=True); time.sleep(60)')
            wrapper=('import subprocess,sys; '
                     'p=subprocess.Popen([sys.executable,"-c",sys.argv[1],"-insecure","+playdemo",sys.argv[2]],'
                     'stdout=sys.stdout,stderr=sys.stderr); ')
            process=subprocess.Popen([sys.executable,'-c',wrapper,child_script,str(demo),'-insecure','+playdemo',str(demo)],
                                     start_new_session=True,stdout=subprocess.PIPE,text=True)
            child=None
            try:
                child=int(process.stdout.readline())
                process.wait(timeout=5)
                self.assertNotIn(child,session_processes(process.pid))
                stop_process(process)
                stat=Path('/proc')/str(child)/'stat'
                self.assertTrue(not stat.exists() or stat.read_text().rsplit(')',1)[1].split()[0]=='Z')
            finally:
                if child:
                    try: os.kill(child,9)
                    except ProcessLookupError: pass
                stop_process(process)
                process.stdout.close()


if __name__ == '__main__':
    unittest.main()
