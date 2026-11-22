from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import TestCase
from unittest.mock import Mock, patch

import app
import cart_backups
import cartridge
import library
from test_cartridge import CAP, gb_rom


class ReplaceRom(TestCase):
    def setUp(self):
        temp = TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.backups = self.root / 'Backups'
        self.own = gb_rom(title=b'FIRST GAME')
        old = gb_rom(size=0x8000, title=b'TEST GAME')
        self.new = old[:-1] + b'\xab'
        self.rom_path = self.root / 'New Test Game.gb'
        self.rom_path.write_bytes(self.new)
        self.game = cartridge.Game('Test Game.gbc', old, b'existing art')
        image = cartridge.build(CAP, [self.game], own_image=self.own)
        self.backup = cart_backups.create_snapshot(self.backups, image, b'sram', title='First Game')
        self.editor = app.App.__new__(app.App)
        self.editor.cart = {'info': cartridge.parse(image), 'games': [self.game], 'image': image,
                            'mode': 'keep', 'backup': self.backup, 'save': b'sram'}
        self.editor.busy = False
        self.editor.cart_list_offset = 1
        self.editor.cart_list = Mock()
        self.editor.cart_list.curselection.return_value = (1,)
        self.editor.draw_cart = Mock()
        self.editor.show_cartridge = Mock()
        self.editor.cart_connection = lambda: ('usb', 'simulated')
        self.editor.run = lambda op, work, folder: work()
        self.sd = self.root / 'SD'
        self.save = self.sd / 'CHROMAGIC' / 'CART' / 'Test Game.SAV'
        self.save.parent.mkdir(parents=True)
        self.save.write_bytes(b'old progress')
        self.editor.cart_save_target = lambda port: library.FolderTarget(self.sd)

    def select_new_rom(self, keep):
        def choice(title, text, **kwargs):
            if title.startswith('Where is'):
                return True
            self.assertIn('Backed up', text)
            snapshots = cart_backups.list_save_snapshots(self.backups)
            self.assertEqual(snapshots[0].path.read_bytes(), self.save.read_bytes())
            return keep

        dialog = Mock()
        dialog.askyesnocancel.side_effect = choice
        with patch.object(app, 'CART_BACKUPS', self.backups), patch.object(app, 'ask_open_filename',
                return_value=str(self.rom_path)), patch.object(app, 'messagebox', dialog, create=True):
            self.editor.cart_replace_rom()
            self.editor.cart_finish_save_replacement()

    def test_replace_keeps_slot_name_and_art_and_backs_up_before_choice(self):
        self.select_new_rom(keep=True)
        target, error = self.editor.cart_target()
        self.assertEqual(error, '')
        result = cartridge.parse(target)
        self.assertEqual(result.games, [cartridge.Game('Test Game.gbc', self.new, self.game.art)])
        self.assertEqual(target[:len(self.own)], self.own)
        self.assertEqual(self.save.read_bytes(), b'old progress')

    def test_cancel_preserves_the_existing_rom_after_the_backup(self):
        self.select_new_rom(keep=None)
        self.assertEqual(self.editor.cart['games'], [self.game])
        self.assertEqual(self.save.read_bytes(), b'old progress')

    def test_start_fresh_queues_cleanup_and_refuses_a_changed_save(self):
        self.select_new_rom(keep=False)
        files, backup = self.editor.cart['game_save_resets']['Test Game.gbc']
        self.assertEqual((backup / 'Test Game.SAV').read_bytes(), b'old progress')
        self.assertEqual(self.save.read_bytes(), b'old progress')
        self.save.write_bytes(b'new progress')
        with self.assertRaisesRegex(ValueError, 'changed after backup'):
            self.editor.cart_check_game_resets(('usb', 'simulated'), [('Test Game.gbc', files, backup)])
        self.assertEqual(self.save.read_bytes(), b'new progress')

    def test_first_game_backup_rejects_any_changed_rom_block_before_writing(self):
        current = self.editor.cart['image']
        new_own = self.own[:-1] + b'\x83'
        target = cartridge.build(CAP, [self.game], own_image=new_own)
        cart = Mock(capacity=CAP, save_size=32768)
        cart.matches.side_effect = lambda image, blocks: 0x100000 not in blocks
        finish = Mock()
        self.editor.cart_open = Mock(return_value=(finish, cart))
        with self.assertRaisesRegex(ValueError, 'changed since'):
            self.editor.cart_write_run('cart_write', current, target, ('usb', 'simulated'),
                                       check=True, backup_own=True)
        cart.read_save.assert_not_called()
        cart.erase.assert_not_called()
        cart.program_blocks.assert_not_called()
        finish.assert_called_once()

    def write_replacement(self, verify):
        self.select_new_rom(keep=False)
        current = self.editor.cart['image']
        target, error = self.editor.cart_target()
        self.assertEqual(error, '')
        cart = Mock(capacity=CAP, save_size=0)
        cart.program_blocks.return_value = 1

        def matches(image, offsets):
            self.assertEqual(self.save.read_bytes(), b'old progress')
            return image == current or verify

        cart.matches.side_effect = matches
        self.editor.cart_open = Mock(return_value=(Mock(), cart))
        files, backup = self.editor.cart['game_save_resets']['Test Game.gbc']
        self.addCleanup(lambda: self.assertEqual((backup / 'Test Game.SAV').read_bytes(), b'old progress'))
        self.editor.cart_write_run('cart_write', current, target, ('usb', 'simulated'), check=True,
                                   game_resets=[('Test Game.gbc', files, backup)])

    def test_verification_happens_before_the_backed_up_save_is_cleared(self):
        self.write_replacement(verify=True)
        self.assertFalse(self.save.exists())

    def test_failed_rom_verification_preserves_the_save(self):
        with self.assertRaisesRegex(ValueError, "doesn't match"):
            self.write_replacement(verify=False)
        self.assertEqual(self.save.read_bytes(), b'old progress')
