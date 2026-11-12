import hashlib, json, shutil, struct, tempfile, unittest
from pathlib import Path
from unittest.mock import patch
import core, fpga

FW = Path(__file__).resolve().parents[1] / 'firmware'
STOCK = {'mcu': 'v0.13.4', 'fpga': '18.37', 'chromatic': 'v4.2'}
ART31 = {'mcu': 'v0.13.4-art31', 'fpga': '18.38', 'chromatic': '1.0.1-art31 (4.2)'}
MB1 = {'mcu': 'vthree-systems-mb1', 'fpga': '50.1', 'chromatic': 'Three systems (MB1)'}


def put_app(data, offset, version, project=b'mcu_fw'):
    data[offset] = 0xE9
    desc = bytearray(256); struct.pack_into('<I', desc, 0, 0xABCD5432)
    v = version.encode() + b'\0'; desc[16:16 + len(v)] = v
    desc[48:48 + len(project) + 1] = project + b'\0'
    data[offset + 0x20:offset + 0x120] = desc


def backup(layout='stock', app='0.13.4-art31', old_app='0.13.4-art31'):
    data = bytearray(b'\xff' * 0x400000)
    data[0x1000] = 0xE9
    table = (FW / ('partition-table-stock.bin' if layout == 'stock' else 'partition-table-three-systems.bin')).read_bytes()
    data[0x8000:0x8000 + len(table)] = table
    if layout == 'stock':
        put_app(data, 0x10000, app)
    else:
        put_app(data, 0x260000, app)
        if old_app:
            put_app(data, 0x10000, old_app)
    return data


def table_bytes(entries):
    body = b''.join(struct.pack('<HBBII16sI', 0x50AA, t, s, o, z, b'p', 0) for t, s, o, z in entries)
    return body + b'\xeb\xeb' + b'\xff' * 14 + hashlib.md5(body).digest()


class Images(unittest.TestCase):
    def test_bundled_images_match_manifest(self):
        self.assertEqual(set(core.check_images()), set(core.IMAGES))

    def test_damaged_bundle_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            for p in FW.iterdir(): shutil.copyfile(p, Path(tmp) / p.name)
            path = Path(tmp) / core.IMAGES['fpga_gb'][0]; d = bytearray(path.read_bytes()); d[-2] ^= 1; path.write_bytes(d)
            with patch.object(core, 'FIRMWARE_DIR', Path(tmp)):
                with self.assertRaises(ValueError): core.check_images()

    def test_bundled_mcu_is_the_release(self):
        data = bytearray(b'\xff' * 0x400000); mcu = core.image('mcu').read_bytes()
        data[0x260000:0x260000 + len(mcu)] = mcu
        self.assertEqual(core.app_info(data, 0x260000), ('mcu_fw', core.RELEASE))
        self.assertLessEqual(len(mcu), core.THREE_SYSTEMS_APP[1])


class Layouts(unittest.TestCase):
    def test_bundled_tables(self):
        self.assertEqual(core.classify(core.parse_table(core.image('table_stock').read_bytes())), 'stock')
        self.assertEqual(core.classify(core.parse_table(core.image('table_three_systems').read_bytes())), 'three-systems')

    def test_backups(self):
        self.assertEqual(core.validate_backup(backup()), ('stock', '0.13.4-art31'))
        self.assertEqual(core.validate_backup(backup(app='0.13.8')), ('stock', '0.13.8'))
        self.assertEqual(core.validate_backup(backup('three-systems', 'three-systems-mb1')), ('three-systems', 'three-systems-mb1'))
        self.assertEqual(core.validate_backup(backup('three-systems', '1.0.0-beta')), ('three-systems', '1.0.0-beta'))

    def test_rejected_backups(self):
        bad = [bytes(100), backup(app='9.9.9'), backup('three-systems', 'esp-at')]
        b = backup(); b[0x1000] = 0xFF; bad.append(b)
        b = backup(); b[0x8010] ^= 1; bad.append(b)
        b = backup(); b[0x10000] = 0; bad.append(b)
        for entries in [[(1, 2, 0x9000, 0x6000), (1, 1, 0xF000, 0x1000), (0, 0, 0x10000, 0x200000)],
                        [(1, 2, 0x9000, 0x6000), (1, 1, 0xF000, 0x1000), (0, 0, 0x10000, 0x100000), (0, 16, 0x110000, 0x100000)],
                        [(1, 2, 0x9000, 0x5000), (1, 1, 0xF000, 0x1000), (0, 0, 0x10000, 0x100000)]]:
            b = backup(); t = table_bytes(entries); b[0x8000:0x8C00] = b'\xff' * 0xC00; b[0x8000:0x8000 + len(t)] = t; bad.append(b)
        for data in bad:
            with self.assertRaises(ValueError): core.validate_backup(data)

    def test_running_versions(self):
        for v in [STOCK, ART31, MB1, core.TARGET, {'mcu': 'v0.13.4-art31-wifi14', 'fpga': '18.38', 'chromatic': 'x'}]:
            self.assertTrue(core.known_running(v), v)
        for v in [{'mcu': 'v9', 'fpga': '18.38'}, {'mcu': 'v0.13.4', 'fpga': '51.1'}]:
            self.assertFalse(core.known_running(v), v)


class Programmer(unittest.TestCase):
    def test_detection(self):
        with patch.object(fpga, 'run', side_effect=['1.1.1', '1 2 0x33aa:0x0120', 'GW5A-25']): fpga.preflight(lambda s: None)
        with patch.object(fpga, 'run', side_effect=['1.1.1', '1 2 0x33aa:0x0120\n1 3 0x33aa:0x0120']):
            with self.assertRaises(ValueError): fpga.preflight(lambda s: None)
        for silent in ['User requested: 6000000 real frequency is 6000000', 'found 0 devices']:
            with patch.object(fpga, 'run', side_effect=['1.1.1', '1 2 0x33aa:0x0120', silent]):
                with self.assertRaises(ValueError) as ctx: fpga.preflight(lambda s: None)
            self.assertIn('unplug', str(ctx.exception))

    def test_retries_until_success(self):
        calls = []
        def run(args, log, timeout=0):
            calls.append(args)
            if len(calls) < 4: raise fpga.ProgrammerError('write to flash failed')
            return 'Verifying write'
        with patch.object(fpga, 'run', side_effect=run), patch.object(fpga, 'reset') as reset:
            self.assertEqual(fpga.flash('img.fs', 0x100000, lambda s: None, sleep=lambda s: None), 4)
            self.assertEqual(reset.call_count, 3)
        self.assertTrue(all(a[a.index('-o') + 1] == '0x100000' and '--verify' in a and '--reset' not in a for a in calls))

    def test_gives_up_after_attempts(self):
        with patch.object(fpga, 'run', side_effect=fpga.ProgrammerError('x')), patch.object(fpga, 'reset'):
            with self.assertRaises(fpga.ProgrammerError):
                fpga.flash('img.fs', 0, lambda s: None, attempts=3, sleep=lambda s: None)

    def test_stuck_programmer_stops_early(self):
        calls = []
        def run(args, log, timeout=0):
            calls.append(args); raise fpga.ProgrammerError('x', 'Error: no device found')
        with patch.object(fpga, 'run', side_effect=run), patch.object(fpga, 'reset'):
            with self.assertRaises(fpga.ProgrammerStuck) as ctx:
                fpga.flash('img.fs', 0, lambda s: None, sleep=lambda s: None)
        self.assertEqual(len(calls), 2); self.assertIn('Unplug the USB cable', str(ctx.exception))

    def test_ordinary_failures_still_retry(self):
        outputs = ['Error: write to flash failed', 'Error: no device found', 'write to flash failed', 'ok']
        def run(args, log, timeout=0):
            out = outputs.pop(0)
            if out != 'ok': raise fpga.ProgrammerError('x', out)
            return out
        with patch.object(fpga, 'run', side_effect=run), patch.object(fpga, 'reset'):
            self.assertEqual(fpga.flash('img.fs', 0, lambda s: None, sleep=lambda s: None), 4)

    def test_rejects_unaligned_slot(self):
        with self.assertRaises(ValueError): fpga.flash('img.fs', 0x1000, lambda s: None)


class Flows(unittest.TestCase):
    def perform(self, op='install', *, layout='stock', app='0.13.4-art31', old_app='0.13.4-art31', running=ART31,
                corrupt_backup=False, verify_backup_fail=False, mcu_fail=False, fpga_fail=False, ports=None,
                after=None, damaged_image=False):
        events = []
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp) / 'backup'
            def tool(port, args):
                if 'read_flash' in args:
                    start, length = int(args[-3], 16), int(args[-2], 16)
                    Path(args[-1]).write_bytes(b'partial' if corrupt_backup else bytes(backup(layout, app, old_app))[start:start + length])
                    if start == 0: events.append('read_flash')
                    return
                if args[0] == 'verify_flash':
                    if verify_backup_fail and args[1] == '0x0': raise ValueError('backup verify failed')
                    events.append(('verify', int(args[1], 16))); return
                if 'erase_flash' in args: events.append('ERASE')
                offset = int(args[args.index('write_flash') + 7], 16)
                events.append(('write', offset, Path(args[-1]).name))
                if mcu_fail and offset == 0x260000: raise ValueError('MCU write failed')
            def flash(path, offset, log, sleep=None):
                events.append(('fpga', offset, Path(path).name))
                if fpga_fail: raise fpga.ProgrammerError('USB disconnected')
                return 2
            versions = [running, after or (core.TARGET if op == 'install' else {'mcu': 'v' + str(old_app), 'fpga': '18.38', 'chromatic': 'x'})]
            def version(port, timeout=20):
                v = versions.pop(0) if versions else None
                if v is None: raise ValueError('No console')
                return v
            fw_patch = patch.object(core, 'FIRMWARE_DIR', FW)
            if damaged_image:
                tmpfw = Path(tmp) / 'fw'; shutil.copytree(FW, tmpfw, copy_function=shutil.copyfile)
                p = tmpfw / core.IMAGES['mcu'][0]; d = bytearray(p.read_bytes()); d[100] ^= 1; p.write_bytes(d)
                fw_patch = patch.object(core, 'FIRMWARE_DIR', tmpfw)
            with fw_patch, patch.object(core, 'device_ports', return_value=['test'] if ports is None else ports), \
                    patch.object(core, 'version', side_effect=version), patch.object(core, 'run_tool', side_effect=tool), \
                    patch.object(core, 'read_flash', side_effect=lambda port, off, size, path, log: tool(port, ['read_flash', hex(off), hex(size), str(path)])), \
                    patch.object(fpga, 'preflight'), patch.object(fpga, 'flash', side_effect=flash), \
                    patch.object(fpga, 'reset', side_effect=lambda log: events.append('reset')):
                error = None
                try: getattr(core, op)('test', folder, lambda s: None, sleep=lambda s: None)
                except ValueError as exc: error = exc
                state = json.loads((folder / 'installation.json').read_text()) if (folder / 'installation.json').exists() else {}
        writes = [e for e in events if isinstance(e, tuple) and e[0] in ('write', 'fpga')]
        return events, writes, error, state.get('stage')

    def test_install_from_stock_layout(self):
        for running in [STOCK, ART31, None]:
            events, writes, error, stage = self.perform(running=running)
            self.assertIsNone(error); self.assertEqual(stage, 'complete')
            self.assertEqual(writes, [('write', 0x260000, core.IMAGES['mcu'][0]),
                                      ('fpga', core.SLOT_GG, core.IMAGES['fpga_gg'][0]),
                                      ('fpga', core.SLOT_GB, core.IMAGES['fpga_gb'][0]),
                                      ('write', 0x8000, core.IMAGES['table_three_systems'][0])])
            self.assertEqual(events[:3], ['read_flash', ('verify', 0), ('write', 0x260000, core.IMAGES['mcu'][0])])
            self.assertIn(('verify', 0x8000), events); self.assertEqual(events[-1], 'reset')

    def test_upgrade_keeps_table_and_never_writes_slot1_behind_multiboot(self):
        for running in [MB1, None]:
            events, writes, error, stage = self.perform(layout='three-systems', app='three-systems-mb1', running=running)
            self.assertIsNone(error); self.assertEqual(stage, 'complete')
            self.assertNotIn(0x8000, [w[1] for w in writes])
            self.assertEqual([w for w in writes if w[0] == 'fpga'],
                             [('fpga', core.SLOT_GB, core.IMAGES['fpga_chromagic'][0]),
                              ('fpga', core.SLOT_GG, core.IMAGES['fpga_gg'][0]),
                              ('fpga', core.SLOT_GB, core.IMAGES['fpga_gb'][0])])

    def test_interrupted_install_with_new_fpga_bounces_too(self):
        _, writes, error, _ = self.perform(running=dict(ART31, fpga='50.1'))
        self.assertIsNone(error)
        self.assertEqual(writes[1], ('fpga', core.SLOT_GB, core.IMAGES['fpga_chromagic'][0]))

    def test_never_touches_bootloader_or_erases(self):
        for kwargs in [{}, {'layout': 'three-systems', 'app': '1.0.0-beta', 'running': core.TARGET}]:
            events, writes, _, _ = self.perform(**kwargs)
            self.assertNotIn('ERASE', events)
            self.assertTrue(all(w[1] >= 0x8000 for w in writes if w[0] == 'write'))

    def test_table_only_after_both_fpga_cores(self):
        _, writes, _, _ = self.perform()
        self.assertGreater(writes.index(('write', 0x8000, core.IMAGES['table_three_systems'][0])),
                           max(i for i, w in enumerate(writes) if w[0] == 'fpga'))

    def test_bad_backup_never_writes(self):
        for kwargs in [{'corrupt_backup': True}, {'verify_backup_fail': True}, {'app': '9.9.9'}]:
            _, writes, error, _ = self.perform(**kwargs)
            self.assertIsNotNone(error); self.assertEqual(writes, [])

    def test_damaged_bundle_never_touches_device(self):
        events, _, error, _ = self.perform(damaged_image=True)
        self.assertIsNotNone(error); self.assertEqual(events, [])

    def test_unknown_firmware_and_multiple_devices(self):
        for kwargs in [{'running': {'mcu': 'v9', 'fpga': '1.0', 'chromatic': 'x'}}, {'ports': ['test', 'other']}]:
            events, _, error, _ = self.perform(**kwargs)
            self.assertIsNotNone(error); self.assertEqual(events, [])

    def test_mcu_failure_stops_before_fpga(self):
        _, writes, error, stage = self.perform(mcu_fail=True)
        self.assertIsNotNone(error); self.assertEqual(stage, 'mcu_write_started')
        self.assertEqual([w[0] for w in writes], ['write'] * core.TOOL_ATTEMPTS)   # retried, never reaches the FPGA

    def test_fpga_failure_leaves_old_table(self):
        _, writes, error, stage = self.perform(fpga_fail=True)
        self.assertIsNotNone(error); self.assertEqual(stage, 'partial_install_fpga_failed')
        self.assertNotIn(0x8000, [w[1] for w in writes])

    def test_either_core_running_counts_as_installed(self):
        _, _, error, stage = self.perform(after=dict(core.TARGET, fpga='50.2'))
        self.assertIsNone(error); self.assertEqual(stage, 'complete')

    def test_unconfirmed_version_is_not_success(self):
        _, _, error, stage = self.perform(after={'mcu': 'v0.13.4-art31', 'fpga': '18.38', 'chromatic': 'x'})
        self.assertIsNotNone(error); self.assertEqual(stage, 'all_images_verified')

    def test_uninstall_restores_chromagic(self):
        events, writes, error, stage = self.perform('uninstall', layout='three-systems', app='1.0.0-beta', running=core.TARGET)
        self.assertIsNone(error); self.assertEqual(stage, 'complete')
        self.assertEqual(writes, [('fpga', core.SLOT_GB, core.IMAGES['fpga_chromagic'][0]),
                                  ('write', 0x8000, core.IMAGES['table_stock'][0])])

    def test_uninstall_unconfirmed_is_not_a_stop(self):
        _, writes, error, stage = self.perform('uninstall', layout='three-systems', app='1.0.0-beta', running=core.TARGET,
                                               after={'mcu': 'v1.0.0-beta', 'fpga': '50.1', 'chromatic': 'x'})
        self.assertIsNone(error); self.assertEqual(stage, 'complete_unconfirmed')

    def test_uninstall_refusals_write_nothing(self):
        for kwargs in [{'layout': 'stock'}, {'layout': 'three-systems', 'app': '1.0.0-beta', 'old_app': None, 'running': core.TARGET}]:
            _, writes, error, _ = self.perform('uninstall', **kwargs)
            self.assertIsNotNone(error); self.assertEqual(writes, [])


class Restore(unittest.TestCase):
    def perform(self, *, layout='stock', app='0.13.8', ports=(['test'],), fpga_fail=False, after=None,
                tamper=False, write_meta=True):
        events = []
        with tempfile.TemporaryDirectory() as tmp:
            saved = Path(tmp) / 'first' / 'mcu-backup.bin'; saved.parent.mkdir()
            data = bytes(backup(layout, app, None)); saved.write_bytes(data)
            if write_meta:
                (saved.parent / 'backup.json').write_text(json.dumps({'sha256': hashlib.sha256(data).hexdigest()}))
            if tamper:
                d = bytearray(data); d[0x300000] ^= 1; saved.write_bytes(d)
            folder = Path(tmp) / 'restore'
            seq = list(ports)
            def tool(port, args):
                if 'read_flash' in args:
                    start, length = int(args[-3], 16), int(args[-2], 16)
                    Path(args[-1]).write_bytes(bytes(length))
                    if start == 0: events.append('read_flash')
                    return
                if args[0] == 'verify_flash': events.append(('verify', int(args[1], 16))); return
                offset = int(args[args.index('write_flash') + 7], 16)
                events.append(('write', offset, len(Path(args[-1]).read_bytes())))
            def flash(path, offset, log, sleep=None):
                events.append(('fpga', offset, Path(path).name))
                if fpga_fail: raise fpga.ProgrammerError('USB disconnected')
                return 1
            def version(port, timeout=20):
                if after is None: raise ValueError('No console')
                return after
            with patch.object(core, 'FIRMWARE_DIR', FW), \
                    patch.object(core, 'device_ports', side_effect=lambda: seq.pop(0) if seq else ['test']), \
                    patch.object(core, 'version', side_effect=version), patch.object(core, 'run_tool', side_effect=tool), \
                    patch.object(core, 'read_flash', side_effect=lambda port, off, size, path, log: tool(port, ['read_flash', hex(off), hex(size), str(path)])), \
                    patch.object(fpga, 'preflight'), patch.object(fpga, 'flash', side_effect=flash), \
                    patch.object(fpga, 'reset'):
                error = None
                try: core.restore(saved, folder, lambda s: None, sleep=lambda s: None)
                except ValueError as exc: error = exc
                state = json.loads((folder / 'installation.json').read_text()) if (folder / 'installation.json').exists() else {}
                leftovers = sorted(p.name for p in folder.iterdir()) if folder.exists() else []
        return events, error, state.get('stage'), leftovers

    def test_restores_fpga_then_mcu_from_0x8000(self):
        events, error, stage, left = self.perform(after={'mcu': 'v0.13.8', 'fpga': '18.38', 'chromatic': 'v4.6'})
        self.assertIsNone(error); self.assertEqual(stage, 'complete')
        self.assertEqual(events, ['read_flash', ('fpga', core.SLOT_GB, core.IMAGES['fpga_chromagic'][0]),
                                  ('write', 0x8000, 0x400000 - 0x8000), ('verify', 0x8000)])
        self.assertNotIn('restore-0x8000.bin', left)

    def test_works_without_serial_port(self):
        events, error, stage, _ = self.perform(ports=([], [], ['test']), after={'mcu': 'v0.13.8', 'fpga': '18.38', 'chromatic': 'x'})
        self.assertIsNone(error); self.assertEqual(stage, 'complete')
        self.assertNotIn('read_flash', events)
        self.assertEqual(events[0][0], 'fpga')

    def test_missing_version_command_is_not_a_failure(self):
        _, error, stage, _ = self.perform(after=None)
        self.assertIsNone(error); self.assertEqual(stage, 'complete_unconfirmed')

    def test_refuses_magic_trick_or_damaged_backup(self):
        for kwargs in [{'layout': 'three-systems', 'app': '1.0.0-beta'}, {'tamper': True}, {'app': '9.9.9'}]:
            events, error, _, left = self.perform(**kwargs)
            self.assertIsNotNone(error, kwargs); self.assertEqual(events, []); self.assertEqual(left, [])

    def test_backup_without_metadata_still_restores(self):
        _, error, _, _ = self.perform(write_meta=False, after={'mcu': 'v0.13.8', 'fpga': '18.38', 'chromatic': 'x'})
        self.assertIsNone(error)

    def test_fpga_failure_stops_before_mcu(self):
        events, error, stage, _ = self.perform(fpga_fail=True)
        self.assertIsNotNone(error); self.assertEqual(stage, 'fpga_failed')
        self.assertFalse([e for e in events if isinstance(e, tuple) and e[0] == 'write'])


class Confirm(unittest.TestCase):
    def test_finds_port_that_appears_late_under_a_new_name(self):
        seq = [[]] * 15 + [['COM4']]
        target = {'mcu': 'v0.13.8', 'fpga': '18.38', 'chromatic': 'v4.6'}
        with patch.object(core, 'device_ports', side_effect=lambda: seq.pop(0) if seq else ['COM4']), \
                patch.object(core, 'version', return_value=target) as version, patch.object(fpga, 'reset'):
            self.assertEqual(core.confirm({'fpga': '18.38', 'mcu': 'v0.13.8'}, lambda s: None, lambda s: None), target)
        version.assert_called_with('COM4')

    def test_gives_up_after_the_window(self):
        with patch.object(core, 'device_ports', return_value=[]), patch.object(fpga, 'reset'):
            with self.assertRaises(ValueError): core.confirm({'fpga': '18.38'}, lambda s: None, lambda s: None)


class CloudFolders(unittest.TestCase):
    def test_detects_synced_folders(self):
        for path, service in [(r'C:\Users\a\OneDrive\Documents\backup', 'OneDrive'),
                              (r'C:\Users\a\OneDrive - Contoso\x', 'OneDrive'),
                              ('/Users/a/Dropbox/chromatic', 'Dropbox'),
                              ('/Users/a/Library/Mobile Documents/com~apple~CloudDocs/x', 'iCloud Drive'),
                              ('/Users/a/Library/CloudStorage/GoogleDrive-a@b.c/x', 'a cloud service'),
                              ('/Users/a/Google Drive/x', 'Google Drive')]:
            self.assertEqual(core.cloud_service(path), service, path)

    def test_local_folders_pass(self):
        for path in [r'C:\MagicTrickTest\backups', '/Users/a/Documents/chromatic', '/Volumes/USB/backup',
                     '/home/a/megamix/backups']:
            self.assertIsNone(core.cloud_service(path), path)


class UsbIds(unittest.TestCase):
    def test_any_player_number_and_unbooted_mcu(self):
        for pid in (0x0100, 0x0101, 0x0102, 0x01FF):
            self.assertTrue(core.is_chromatic(0x374e, pid), hex(pid))
        for vid, pid in ((0x374e, 0x0200), (0x33aa, 0x0120), (0x374e, None), (None, None)):
            self.assertFalse(core.is_chromatic(vid, pid), (vid, pid))


class ResilientRead(unittest.TestCase):
    def session(self, fail_at=(), always_fail=False):
        from esptool.util import FatalError
        data = bytes(range(256)) * (0x400000 // 256)
        state = {'connects': 0, 'reads': []}
        class Esp:
            _port = type('P', (), {'close': lambda self: None})()
            def run_stub(self): return self
            def flash_spi_attach(self, v): pass
            def flash_set_parameters(self, n): pass
            def hard_reset(self): pass
            def read_flash(self, pos, n):
                state['reads'].append(pos)
                if always_fail or (pos in fail_at and state['reads'].count(pos) == 1):
                    raise FatalError('Corrupt data, expected 0x1000 bytes but received 0xfff bytes')
                return data[pos:pos + n]
        def detect(port, baud, mode):
            state['connects'] += 1; return Esp()
        import io, contextlib
        with tempfile.TemporaryDirectory() as tmp, patch('esptool.cmds.detect_chip', side_effect=detect), \
                patch.object(core.time, 'sleep'), contextlib.redirect_stdout(io.StringIO()):
            out = Path(tmp) / 'b.bin'
            try:
                core.read_flash_session('COM3', 0, 0x400000, out)
                return data, out.read_bytes(), state, None
            except FatalError as exc:
                return data, out.exists(), state, exc

    def test_failed_chunks_are_reread_on_a_new_connection(self):
        data, got, state, err = self.session(fail_at=(0x80000, 0x3F0000))
        self.assertIsNone(err); self.assertEqual(got, data)
        self.assertEqual(state['connects'], 3)
        self.assertEqual(len(state['reads']), 64 + 2)

    def test_persistent_failure_writes_nothing(self):
        _, exists, state, err = self.session(always_fail=True)
        self.assertIsNotNone(err); self.assertFalse(exists)
        self.assertEqual(state['connects'], core.READ_ATTEMPTS)

    def test_parent_checks_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'b.bin'
            with patch.object(core, 'run_child', side_effect=lambda cmd, t=None: Path(cmd[-1]).write_bytes(bytes(0x400000))):
                core.read_flash('COM3', 0, 0x400000, out, print)
            with patch.object(core, 'run_child', side_effect=lambda cmd, t=None: Path(cmd[-1]).write_bytes(bytes(10))):
                with self.assertRaises(ValueError): core.read_flash('COM3', 0, 0x400000, out, print)


class EsptoolProcess(unittest.TestCase):
    def fake(self, code):
        import sys
        return patch.object(core, 'esptool_command', return_value=[sys.executable, '-c', code])

    def test_success_streams_output(self):
        import io, contextlib
        buf = io.StringIO()
        with self.fake('import sys; print("args", sys.argv[1:]); print("Hash of data verified.")'), contextlib.redirect_stdout(buf):
            core.run_tool('COM3', ['flash_id'])
        self.assertIn('Hash of data verified.', buf.getvalue())
        self.assertIn("'--port', 'COM3'", buf.getvalue())

    def test_failure_raises_with_last_line(self):
        with self.fake('print("A fatal error occurred: Failed to connect"); raise SystemExit(2)'):
            with self.assertRaises(ValueError) as ctx: core.run_tool('COM3', ['flash_id'])
        self.assertIn('Failed to connect', str(ctx.exception))

    def test_tool_hanging_after_fatal_error_is_stopped(self):
        import time
        start = time.monotonic()
        with self.fake('import time; print("A fatal error occurred: Corrupt data", flush=True); time.sleep(60)'), \
                patch.object(core, 'FATAL_EXIT_GRACE', 1):
            with self.assertRaises(ValueError) as ctx: core.run_tool('COM3', ['read_flash'])
        self.assertIn('Corrupt data', str(ctx.exception)); self.assertLess(time.monotonic() - start, 10)

    def test_silent_tool_is_stopped(self):
        import time
        start = time.monotonic()
        with self.fake('import time; print("Connecting...", flush=True); time.sleep(60)'):
            with self.assertRaises(ValueError) as ctx: core.run_tool('COM3', ['flash_id'], idle_timeout=1)
        self.assertIn('stopped responding', str(ctx.exception))
        self.assertLess(time.monotonic() - start, 10)


class Reconnect(unittest.TestCase):
    def test_waits_for_new_port_name(self):
        seq = [[], [], ['/dev/new']]
        with patch.object(core, 'device_ports', side_effect=lambda: seq.pop(0) if seq else ['/dev/new']), \
                patch.object(fpga, 'reset') as reset:
            self.assertEqual(core.wait_for_device(lambda s: None, lambda s: None), '/dev/new')
            reset.assert_not_called()

    def test_resets_fpga_then_gives_up(self):
        with patch.object(core, 'device_ports', return_value=[]), patch.object(fpga, 'reset') as reset, \
                patch.object(core.time, 'monotonic', side_effect=[0, 0, 100, 0, 0, 100]):
            with self.assertRaises(ValueError): core.wait_for_device(lambda s: None, lambda s: None)
            reset.assert_called_once()

class LinuxPortAccess(unittest.TestCase):
    def test_unreadable_port_gets_a_clear_message_on_linux(self):
        with tempfile.TemporaryDirectory() as d:
            port = Path(d) / 'ttyACM0'
            port.write_bytes(b'')
            port.chmod(0)
            try:
                with self.assertRaisesRegex(ValueError, 'USB permissions'):
                    core.check_port_access(str(port), 'linux')
                core.check_port_access(str(port), 'darwin')   # other systems: left to the normal path
            finally:
                port.chmod(0o600)
            core.check_port_access(str(port), 'linux')   # accessible: fine
        core.check_port_access('/dev/does-not-exist', 'linux')   # missing: handled elsewhere

class BackupWaitsOutProgrammingModeFailures(unittest.TestCase):
    BOOT_MODE = ValueError('The MCU tool failed: A fatal error occurred: Failed to connect to Espressif device: '
                           'Wrong boot mode detected (0x3f)!')

    def _session(self, d, slept):
        return core.Session('/dev/old', Path(d) / 'b', lambda *a: None, 'uninstall', sleep=slept.append)

    def _patches(self, read):
        return (patch.object(core, 'read_flash', side_effect=read), patch.object(core.fpga, 'reset'),
                patch.object(core, 'wait_for_device', return_value='/dev/new'),
                patch.object(core, 'validate_backup', return_value=('three-systems', 'x')),
                patch.object(core, 'run_tool'))

    def test_waits_and_retries_on_the_new_port_without_restarting_fpga(self):
        calls, slept = [], []
        def read(port, offset, size, path, log):
            calls.append(port)
            if len(calls) == 1:
                raise self.BOOT_MODE
            Path(path).write_bytes(b'x')
        with tempfile.TemporaryDirectory() as d:
            s = self._session(d, slept)
            a, reset, c, e, f = self._patches(read)
            with a, reset as r, c, e, f:
                s.backup()
            r.assert_not_called()
            self.assertEqual(calls, ['/dev/old', '/dev/new'])
            self.assertIn(core.BACKUP_SETTLE, slept)
            self.assertEqual(s.port, '/dev/new')

    def test_gives_clear_guidance_after_the_last_round(self):
        with tempfile.TemporaryDirectory() as d:
            s = self._session(d, [])
            a, b, c, e, f = self._patches(self.BOOT_MODE)
            with a as read, b, c, e, f:
                with self.assertRaisesRegex(ValueError, 'stay in its menu'):
                    s.backup()
            self.assertEqual(read.call_count, core.BACKUP_ROUNDS)

    def test_other_read_failures_are_not_retried(self):
        with tempfile.TemporaryDirectory() as d:
            s = self._session(d, [])
            a, b, c, e, f = self._patches(ValueError('Reading the MCU flash did not complete.'))
            with a as read, b, c, e, f:
                with self.assertRaisesRegex(ValueError, 'did not complete'):
                    s.backup()
            self.assertEqual(read.call_count, 1)


if __name__ == '__main__':
    unittest.main()


class StockConsoleWindow(unittest.TestCase):
    """The stock console only answers for a few seconds after a restart (seen on Windows)."""

    def fake_serial(self, answers_without_reset):
        reply = b'mcu> fwversion { "mcu": "v0.13.8", "fpga": "18.38", "chromatic": "v4.6" }\r\nmcu> '
        log = []

        class Port:
            def __init__(self, **kw):
                self.baudrate = kw['baudrate']; self.rts = False; self.dtr = False
                self.pending = b''; self.restarted = False
                self._rts = False
            def __setattr__(self, name, value):
                if name == 'rts' and getattr(self, '_rts', False) and not value:
                    object.__setattr__(self, 'restarted', True); log.append('restart')
                if name == 'rts':
                    object.__setattr__(self, '_rts', value)
                object.__setattr__(self, name, value)
            def open(self):
                log.append(('open', self.baudrate))
            def read(self, n):
                out, self.pending = self.pending, b''
                return out
            def write(self, data):
                if self.baudrate == 115200 and (self.restarted or answers_without_reset):
                    self.pending += reply
            def flush(self):
                pass
            def close(self):
                pass
        return Port, log

    def test_restarts_and_asks_when_plain_read_is_ignored(self):
        Port, log = self.fake_serial(answers_without_reset=False)
        with patch('serial.Serial', Port), patch.object(core, 'RESET_LISTEN', 1):
            v = core.version('COM4', timeout=1)
        self.assertEqual(v['mcu'], 'v0.13.8')
        self.assertIn('restart', log)

    def test_no_restart_when_console_answers(self):
        Port, log = self.fake_serial(answers_without_reset=True)
        with patch('serial.Serial', Port):
            v = core.version('COM4', timeout=1)
        self.assertEqual(v['fpga'], '18.38')
        self.assertNotIn('restart', log)

    def test_waits_for_the_fpga_version_after_a_restart(self):
        Port, log = self.fake_serial(answers_without_reset=True)
        replies = [b'{ "mcu": "v1.0.0-beta.1", "fpga": "0.0", "chromatic": "Magic Trick 1.0 beta (ChroMagic)" }\r\n',
                   b'{ "mcu": "v1.0.0-beta.1", "fpga": "50.2", "chromatic": "Magic Trick 1.0 beta (ChroMagic)" }\r\n']
        def write(port, data):
            if replies:
                port.pending += replies.pop(0)
        with patch('serial.Serial', Port), patch.object(Port, 'write', write):
            v = core.version('COM4', timeout=3)
        self.assertEqual(v['fpga'], '50.2')

    def test_trace_failure_does_not_end_attempt(self):
        Port, log = self.fake_serial(answers_without_reset=False)
        def bad_trace(text):
            text.encode('cp1252')   # like print() to a cp1252 console
            if 'mcu' in text:
                raise UnicodeEncodeError('charmap', '�', 0, 1, 'boom')
        with patch('serial.Serial', Port), patch.object(core, 'RESET_LISTEN', 1):
            v = core.version('COM4', timeout=1, trace=bad_trace)
        self.assertEqual(v['mcu'], 'v0.13.8')


class AlmostDone(unittest.TestCase):
    def test_download_mode_stops_the_final_check_early(self):
        logs = []
        with patch.object(core, 'device_ports', return_value=['COM5']), patch.object(fpga, 'reset'), \
                patch.object(core, 'version', side_effect=core.DownloadMode('off and on')) as version:
            with self.assertRaises(ValueError):
                core.confirm({'mcu': 'v0.13.8'}, logs.append, lambda s: None)
        self.assertEqual(version.call_count, 1)
        self.assertTrue(any('download mode' in line for line in logs))

    def test_check_again_marks_complete(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'installation.json'
            path.write_text(json.dumps({'stage': 'complete_unconfirmed', 'expected': {'fpga': '18.38', 'mcu': 'v0.13.8'}}))
            with patch.object(core, 'device_ports', return_value=['COM4']), \
                    patch.object(core, 'version', return_value={'mcu': 'v0.13.8', 'fpga': '18.38', 'chromatic': 'v4.6'}):
                ok, _ = core.recheck(d)
            self.assertTrue(ok)
            self.assertEqual(json.loads(path.read_text())['stage'], 'complete')

    def test_check_again_wrong_versions(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'installation.json'
            path.write_text(json.dumps({'stage': 'complete_unconfirmed', 'expected': {'fpga': '18.38', 'mcu': 'v0.13.8'}}))
            with patch.object(core, 'device_ports', return_value=['COM4']), \
                    patch.object(core, 'version', return_value={'mcu': 'v1.0.0-beta', 'fpga': '50.1', 'chromatic': 'x'}):
                ok, found = core.recheck(d)
            self.assertFalse(ok)
            self.assertEqual(json.loads(path.read_text())['stage'], 'complete_unconfirmed')

