import hashlib, struct, unittest
import core
class Safety(unittest.TestCase):
    def test_version_gate(self):
        for v in ['18.37','18.39','',None]:
            self.assertFalse(core.compatible({'fpga':v,'mcu':'v0.13.4-art31','chromatic':'1.0.1-art31 (4.2)'}))
        self.assertFalse(core.compatible({'fpga':'18.38','mcu':'v0.13.4','chromatic':'4.2'}))
    def test_corrupt_image(self):
        with self.assertRaises(ValueError): core.validate_image(b'bad')
    def backup(self,offset=0x10000,size=0x100000):
        data=bytearray(b'\xff'*0x400000)
        entry=struct.pack('<HBBII16sI',0x50AA,0,0,offset,size,b'factory',0)
        data[0x8000:0x8040]=entry+b'\xeb\xeb'+b'\xff'*14+hashlib.md5(entry).digest()
        data[0x10000]=0xe9
        return data
    def test_layout(self): core.validate_backup(self.backup())
    def test_wrong_layout(self):
        for offset,size in [(0,0x100000),(0x10000,0x200000)]:
            with self.assertRaises(ValueError): core.validate_backup(self.backup(offset,size))
    def test_backup_corruption(self):
        data=self.backup(); data[0x8010]^=1
        with self.assertRaises(ValueError): core.validate_backup(data)
        with self.assertRaises(ValueError): core.validate_backup(data[:100])
if __name__=='__main__': unittest.main()

class InstallationOrder(unittest.TestCase):
    def test_wrong_device_never_flashes(self):
        from unittest.mock import patch
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as tmp:
            image=Path(tmp)/'image'; image.write_bytes(b'test')
            with patch.object(core,'validate_image'), patch.object(core,'version',return_value={'fpga':'0'}), patch.object(core,'run_tool') as tool:
                with self.assertRaises(ValueError): core.install('test',image,Path(tmp)/'backup',lambda s:None)
                tool.assert_not_called()
    def test_failed_backup_never_writes(self):
        from unittest.mock import patch
        import tempfile
        from pathlib import Path
        v={'mcu':'v0.13.4-art31','fpga':'18.38','chromatic':'1.0.1-art31 (4.2)'}
        calls=[]
        def fake(port,args):
            calls.append(args)
            if args[0]=='read_flash': Path(args[-1]).write_bytes(b'partial')
        with tempfile.TemporaryDirectory() as tmp:
            image=Path(tmp)/'image'; image.write_bytes(b'test')
            with patch.object(core,'validate_image'),patch.object(core,'version',return_value=v),patch.object(core,'run_tool',side_effect=fake):
                with self.assertRaises(ValueError): core.install('test',image,Path(tmp)/'backup',lambda s:None)
                self.assertEqual(len(calls),1)
                self.assertEqual(calls[0][0],'read_flash')
