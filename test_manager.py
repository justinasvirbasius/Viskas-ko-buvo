import json
import tempfile
import time
import unittest
from pathlib import Path

from server_script_manager.config import load_config
from server_script_manager.manager import ScriptManager


class ManagerTests(unittest.TestCase):
    def make_config(self, root: Path, content: str) -> Path:
        config = root / "ssm.toml"
        config.write_text(content, encoding="utf-8")
        return config

    def test_preview_and_two_gate_cleanup(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = root / "data"
            cache = data / "cache"
            cache.mkdir(parents=True)
            old = cache / "old.tmp"
            old.write_text("stale", encoding="utf-8")
            old_time = time.time() - 40 * 86400
            import os
            os.utime(old, (old_time, old_time))
            config_path = self.make_config(root, '''
[manager]
mode="propose"
[policy]
allowed_roots=["./data"]
cleanup_roots=["./data/cache"]
cleanup_older_than_days=30
allow_destructive=true
''')
            manager = ScriptManager(load_config(config_path))
            preview = manager.run("cleanup")["result"]
            self.assertEqual(preview["candidate_count"], 1)
            self.assertTrue(old.exists())
            with self.assertRaises(PermissionError):
                manager.run("cleanup", apply=True)
            self.assertTrue(old.exists())

    def test_apply_requires_mode_policy_and_cli_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "data/cache").mkdir(parents=True)
            config_path = self.make_config(root, '''
[manager]
mode="apply"
[policy]
allowed_roots=["./data"]
cleanup_roots=["./data/cache"]
allow_destructive=true
''')
            manager = ScriptManager(load_config(config_path))
            result = manager.run("cleanup")["result"]
            self.assertEqual(result["mode"], "preview")
            self.assertEqual(manager.run("inventory")["status"], "ok")

    def test_apply_deletes_only_after_both_config_gates_and_cli_switch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cache = root / "data/cache"
            cache.mkdir(parents=True)
            old = cache / "old.tmp"
            old.write_text("stale", encoding="utf-8")
            old_time = time.time() - 40 * 86400
            import os
            os.utime(old, (old_time, old_time))
            config_path = self.make_config(root, '''
[manager]
mode="apply"
[policy]
allowed_roots=["./data"]
cleanup_roots=["./data/cache"]
cleanup_older_than_days=30
allow_destructive=true
''')
            manager = ScriptManager(load_config(config_path))
            preview = manager.run("cleanup")["result"]
            self.assertEqual(preview["candidate_count"], 1)
            self.assertTrue(old.exists())
            applied = manager.run("cleanup", apply=True)["result"]
            self.assertEqual(applied["removed"], [str(old)])
            self.assertFalse(old.exists())

    def test_cleanup_root_must_be_confined(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config_path = self.make_config(root, '''
[policy]
allowed_roots=["./data"]
cleanup_roots=["../outside"]
''')
            with self.assertRaises(ValueError):
                load_config(config_path)

    def test_forecast_flags_resource_overlap_and_unknown(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "data").mkdir()
            config_path = self.make_config(root, '''
[policy]
allowed_roots=["./data"]
min_free_bytes=0
[[jobs]]
name="a"
routine="disk_report"
every_seconds=60
reads=["disk"]
writes=["index"]
[[jobs]]
name="b"
routine="inventory"
every_seconds=120
reads=["index"]
writes=[]
[[jobs]]
name="c"
routine="not-real"
every_seconds=10
''')
            forecast = ScriptManager(load_config(config_path)).forecast()
            self.assertEqual(len(forecast["resource_collisions"]), 1)
            self.assertEqual(forecast["unknown_routines"][0]["routine"], "not-real")
            self.assertEqual(forecast["decision"], "review-required")

    def test_inventory_counts_non_symlink_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = root / "data"
            data.mkdir()
            (data / "one.txt").write_text("abc", encoding="utf-8")
            config_path = self.make_config(root, '''
[policy]
allowed_roots=["./data"]
''')
            result = ScriptManager(load_config(config_path)).run("inventory")["result"]
            self.assertEqual(result["files"], 1)
            self.assertEqual(result["bytes"], 3)

    def test_output_limit_trims_verbose_lists(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = root / "data"
            data.mkdir()
            for i in range(60):
                (data / ("log-" + str(i) + "-" + "x" * 80 + ".log")).write_text("x", encoding="utf-8")
                old_time = time.time() - 45 * 86400
                import os
                os.utime(data / ("log-" + str(i) + "-" + "x" * 80 + ".log"), (old_time, old_time))
            config_path = self.make_config(root, '''
[policy]
allowed_roots=["./data"]
cleanup_older_than_days=30
max_output_bytes=1024
''')
            response = ScriptManager(load_config(config_path)).run("log_age_report")
            self.assertTrue(response["result"].get("old_logs_omitted") or
                            response["result"].get("summary"))

    def test_reporitor_ticker_forecast_reports_missing_powershell(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = root / "data"
            ticker = root / "reporitor-ticker"
            data.mkdir()
            ticker.mkdir()
            (ticker / "Start-ReporitorTicker.ps1").write_text("Write-Output 'tick'\\n", encoding="utf-8")
            config_path = self.make_config(root, '''
[policy]
allowed_roots=["./data"]
min_free_bytes=0
[reporitor_ticker]
enabled=true
module_path="./reporitor-ticker"
root="./ticker-state"
executable="definitely-not-powershell"
repository="owner/repo"
[[jobs]]
name="repo-ticker"
routine="reporitor_ticker"
every_seconds=60
reads=["repository"]
writes=["ticker-state"]
''')
            forecast = ScriptManager(load_config(config_path)).forecast()
            self.assertEqual(forecast["unknown_routines"], [])
            self.assertIn(forecast["reporitor_ticker"]["status"], {"ready", "unavailable"})
            self.assertEqual(forecast["reporitor_ticker"]["repository"], "owner/repo")


if __name__ == "__main__":
    unittest.main()
