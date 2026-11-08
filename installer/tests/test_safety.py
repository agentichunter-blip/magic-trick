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
