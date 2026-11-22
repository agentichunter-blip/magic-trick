from base64 import b64encode
from unittest import TestCase
from unittest.mock import Mock

import cartridge


class UsbBackup(TestCase):
    def read_save(self, blocks):
        usb = Mock()
        usb._record.side_effect = ['CART capacity=2097152 save=2048', *blocks]
        return cartridge.CartUsb(usb).read_save()

    def test_reads_only_a_complete_ordered_save(self):
        first = bytes(cartridge.BLOCK)
        second = b'\xa5' * cartridge.BLOCK
        blocks = ['CDATA 0 ' + b64encode(first).decode(), 'CDATA 400 ' + b64encode(second).decode()]
        self.assertEqual(self.read_save(blocks), first + second)
        with self.assertRaisesRegex(cartridge.CartError, 'out-of-order'):
            self.read_save([blocks[0], blocks[0]])

    def test_rejects_a_corrupt_or_short_save_block(self):
        with self.assertRaisesRegex(cartridge.CartError, 'Invalid cartridge data'):
            self.read_save(['CDATA 0 !!'])
        with self.assertRaisesRegex(cartridge.CartError, 'Incomplete'):
            self.read_save(['CDATA 0 ' + b64encode(b'\0').decode()])
