"""Deterministic lifecycle checks; no model processes or network requests launched."""
import importlib.util
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location('editor_host', Path(__file__).parents[1] / 'editor-host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)

class Lifecycle(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        bank = self.root / 'Assets/StreamingAssets/Semantic'
        bank.mkdir(parents=True)
        (bank / 'pell-cask.tsv').write_text('prompt\tresponse\temote_tag\nhello\thello\twave\n')
        (bank / 'pell-cask.profile.json').write_text('{}')
        self.s = host.Supervisor(self.root, self.root, 'test-owner', '4b')
        self.s.log = Mock()
        self.s.status = Mock()
        self.health = dict(service='semantic-unity', protocol=1, ready=True, bankSha256=self.s.bank_hash, profileSha256=self.s.profile_hash, modelAlias=self.s.alias)
    def routes(self, port, route):
        if port == 5192: return self.health
        return {'status':'ok'} if route == '/health' else {'data':[{'id':self.s.alias}]}
    def test_healthy_external_services_reused(self):
        with patch.object(host, 'get_json', side_effect=self.routes), patch.object(host, 'listeners', return_value={555}), patch.object(host.subprocess, 'Popen') as launch:
            self.s.step()
            launch.assert_not_called()
            self.assertTrue(self.s.ever_ready)
    def test_absent_starts_model_and_build_once(self):
        process = Mock(pid=321); process.poll.return_value=None
        with patch.object(host,'get_json',return_value=None), patch.object(host,'listeners',return_value=set()), patch.object(host.subprocess,'Popen',return_value=process) as launch:
            self.s.step(); self.s.step()
            self.assertEqual(2,launch.call_count)
            self.assertEqual({'model':1,'build':1},self.s.attempts)
    def test_loading_owned_model_waits(self):
        child=Mock(pid=12);child.poll.return_value=None;self.s.children['model']=child
        with patch.object(host,'get_json',return_value=None),patch.object(host,'listeners',side_effect=lambda p: {12} if p==5194 else set()),patch.object(host.subprocess,'Popen') as launch:
            self.s.built=True;self.s.step();launch.assert_not_called()
            self.assertFalse(self.s.ever_ready)
    def test_foreign_occupied_port_never_replaced(self):
        with patch.object(host,'get_json',return_value=None),patch.object(host,'listeners',return_value={999}),patch.object(host.subprocess,'Popen') as launch:
            with self.assertRaisesRegex(RuntimeError,'occupied'): self.s.step()
            launch.assert_not_called()
        self.s.cleanup() # No remembered child: nothing may be killed.
    def test_mismatched_bank_rejected(self):
        self.health['bankSha256']='wrong'
        with self.assertRaisesRegex(RuntimeError,'different dialogue bank'):
            host.verify_bridge(self.health,self.s.bank_hash,self.s.profile_hash,self.s.alias)
    def test_model_failure_retains_existing_bridge(self):
        self.health['ready']=False;self.s.ever_ready=True;self.s.deadline=None
        with patch.object(host,'get_json',side_effect=lambda p,r:self.health if p==5192 else None),patch.object(host,'listeners',side_effect=lambda p:{777} if p==5192 else set()),patch.object(host.subprocess,'Popen') as launch:
            self.s.step()
            self.assertEqual(1,launch.call_count)
            self.assertEqual({'model':1},self.s.attempts)
    def test_retries_are_bounded(self):
        self.s.attempts['model']=3
        with self.assertRaisesRegex(RuntimeError,'two recovery'): self.s.launch('model',['unused'])
    def test_readiness_deadline(self):
        self.s.deadline=time.monotonic()-1
        with patch.object(host,'get_json',return_value=None),patch.object(host,'listeners',return_value=set()):
            with self.assertRaisesRegex(RuntimeError,'within 120'): self.s.step()
    def test_missing_prerequisite_actionable(self):
        with self.assertRaisesRegex(RuntimeError,'Missing prerequisite'): self.s.preflight()
    def test_lease_expiration_and_epoch_reset(self):
        host.atomic_json(self.s.lease_path,{'owner':'test-owner','epoch':'one','expiresAt':time.time()+10})
        self.s.attempts['model']=3
        self.assertTrue(self.s.lease());self.assertEqual({},self.s.attempts)
        host.atomic_json(self.s.lease_path,{'owner':'test-owner','epoch':'one','expiresAt':0})
        self.assertFalse(self.s.lease())
    def test_cleanup_refuses_nonchild_pid(self):
        child=Mock(pid=42);child.poll.return_value=None;self.s.children['model']=child
        with patch.object(host.subprocess,'run',return_value=Mock(stdout='1 llama-server Qwen3-4B-Q4_K_M.gguf')):
            self.s.cleanup()
        child.terminate.assert_not_called();child.kill.assert_not_called()

if __name__=='__main__': unittest.main()
