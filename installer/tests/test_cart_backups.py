import os
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import patch

import cart_backups as backups


class CartridgeBackupsTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.folder = Path(temporary.name)

    def test_concurrent_reads_keep_each_version_with_matching_save(self):
        saves = [b'first save', b'second save', b'third save']
        with ThreadPoolExecutor() as workers:
            paths = list(workers.map(
                lambda save: backups.create_snapshot(self.folder, b'same image', save, title='GAME'),
                saves,
            ))
        self.assertEqual(len(set(paths)), len(saves))
        for path, save in zip(paths, saves):
            self.assertEqual(path.read_bytes(), b'same image')
            self.assertEqual(path.with_suffix('.sav').read_bytes(), save)
        self.assertEqual({row.path for row in backups.list_save_snapshots(self.folder)},
                         {path.with_suffix('.sav') for path in paths})

    def test_failed_snapshot_does_not_publish_or_change_old_backup(self):
        older = backups.create_snapshot(self.folder, b'older ROM', b'older SRAM')
        write = backups._write_new

        def fail_save(path, data):
            if path.suffix == '.sav':
                raise OSError('Storage unavailable')
            write(path, data)

        with patch.object(backups, '_write_new', side_effect=fail_save):
            with self.assertRaises(OSError):
                backups.create_snapshot(self.folder, b'newer ROM', b'newer SRAM')
        self.assertEqual(list(self.folder.iterdir()), [older.parent])
        self.assertEqual(older.read_bytes(), b'older ROM')
        self.assertEqual(older.with_suffix('.sav').read_bytes(), b'older SRAM')

    def test_history_includes_legacy_pair_and_skips_unpublished_files(self):
        legacy = self.folder / 'old 2025-06-15 1234ABCD.bin'
        legacy.write_bytes(b'legacy ROM')
        save = legacy.with_suffix('.sav')
        save.write_bytes(b'legacy SRAM')
        os.utime(save, ns=(1, 1))
        orphan = self.folder / 'orphan.sav'
        orphan.write_bytes(b'no ROM')
        pending = self.folder / '.pending-interrupted'
        pending.mkdir()
        (pending / 'pending.bin').write_bytes(b'partial ROM')
        (pending / 'pending.sav').write_bytes(b'partial save')
        newer = backups.create_snapshot(self.folder, b'new ROM', b'new SRAM')
        no_save = backups.create_snapshot(self.folder, b'ROM only')
        self.assertTrue(no_save.is_file())
        self.assertFalse(no_save.with_suffix('.sav').exists())
        self.assertEqual([row.path for row in backups.list_save_snapshots(self.folder)],
                         [newer.with_suffix('.sav'), save])

    def test_sd_save_and_rtc_keep_spelling_and_save_can_be_exported(self):
        files = {'Example.GBC.SAV': b'game progress', 'Example.GBC.rtc': b'clock'}
        snapshot = backups.create_game_snapshot(self.folder, 'Example.GBC', files)
        self.assertEqual({path.name: path.read_bytes() for path in snapshot.iterdir()}, files)
        records = backups.list_save_snapshots(self.folder)
        self.assertEqual([(entry.source, entry.path.name) for entry in records],
                         [('SD card', 'Example.GBC.SAV')])
        exported = self.folder / 'Copy.sav'
        backups.export_save(records[0], exported)
        self.assertEqual(exported.read_bytes(), files['Example.GBC.SAV'])
        with self.assertRaises(FileExistsError):
            backups.export_save(records[0], exported)
        self.assertEqual(exported.read_bytes(), files['Example.GBC.SAV'])
        self.assertIsNone(backups.create_game_snapshot(self.folder, 'empty.gbc', {}))

    def test_sd_snapshot_rejects_paths_and_case_colliding_names(self):
        for files in ({'../escape.sav': b'1'}, {r'folder\escape.sav': b'2'},
                      {'Example.SAV': b'1', 'example.sav': b'2'}):
            with self.subTest(files=files):
                with self.assertRaises(ValueError):
                    backups.create_game_snapshot(self.folder, 'Example', files)
        self.assertEqual(list(self.folder.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
