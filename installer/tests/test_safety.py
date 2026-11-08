import hashlib, struct, unittest, tempfile, json
from pathlib import Path
from unittest.mock import patch
import core, fpga
TARGET={'mcu':'v0.13.4-art31','fpga':'18.38','chromatic':'1.0.1-art31 (4.2)'}
STOCK={'mcu':'v0.13.4','fpga':'18.37','chromatic':'v4.2'}
def backup():
    data=bytearray(b'\xff'*0x400000)
    entry=struct.pack('<HBBII16sI',0x50AA,0,0,0x10000,0x100000,b'factory',0)
    data[0x8000:0x8040]=entry+b'\xeb\xeb'+b'\xff'*14+hashlib.md5(entry).digest()
    data[0x10000]=0xe9
    desc=bytearray(256);struct.pack_into('<I',desc,0,0xabcd5432)
    desc[16:23]=b'0.13.4\0';desc[48:55]=b'mcu_fw\0';data[0x10020:0x10120]=desc
    return data
class Safety(unittest.TestCase):
    def test_versions(self):
        for v in [STOCK,TARGET,{'mcu':'v0.13.8','fpga':'18.50','chromatic':'v4.6'}]: self.assertTrue(core.supported_source(v))
        self.assertFalse(core.supported_source({'mcu':'v9','fpga':'18.38','chromatic':'v4.6'}))
    def test_image_corruption(self):
        with self.assertRaises(ValueError): core.validate_image(b'bad')
    def test_backup(self):
        core.validate_backup(backup());self.assertEqual(core.identify_backup(backup()),'0.13.4')
    def test_backup_corruption(self):
        b=backup();b[0x8010]^=1
        with self.assertRaises(ValueError): core.validate_backup(b)
        with self.assertRaises(ValueError): core.validate_backup(b[:100])
        b=backup();b[0x10020]=0
        with self.assertRaises(ValueError):core.identify_backup(b)
    def test_wrong_partition_layout(self):
        for offset,size in [(0,0x100000),(0x10000,0x200000)]:
            b=backup()
            entry=struct.pack('<HBBII16sI',0x50AA,0,0,offset,size,b'factory',0)
            b[0x8000:0x8040]=entry+b'\xeb\xeb'+b'\xff'*14+hashlib.md5(entry).digest()
            with self.assertRaises(ValueError):core.validate_backup(b)
    def test_unknown_backup_app(self):
        b=backup();b[0x10050:0x10057]=b'bad_fw\0'
        with self.assertRaises(ValueError):core.identify_backup(b)
    def test_programmer_detection(self):
        with patch.object(fpga,'run',side_effect=['1.1.1','1 2 0x33aa:0x0120','GW5A-25']):fpga.preflight(lambda s:None)
        with patch.object(fpga,'run',side_effect=['1.1.1','1 2 0x33aa:0x0120\n1 3 0x33aa:0x0120']):
            with self.assertRaises(ValueError):fpga.preflight(lambda s:None)
class InstallationOrder(unittest.TestCase):
    def perform(self, *, corrupt_backup=False, fpga_fail=False, corrupt_fpga=False, source=STOCK, read_fail=False, mcu_fail=False, ports=None):
        events=[]
        with tempfile.TemporaryDirectory() as tmp:
            image=Path(tmp)/'mcu';image.write_bytes(b'mcu')
            f=Path(tmp)/'fpga';f.write_bytes(b'fpga')
            folder=Path(tmp)/'backup'
            def tool(port,args):
                if 'read_flash' in args: Path(args[-1]).write_bytes(b'partial' if corrupt_backup else backup())
                if read_fail and args[0]=='verify_flash' and args[1]=='0x0': raise ValueError('backup verify failed')
                events.append('write_mcu' if 'write_flash' in args else args[0])
                if mcu_fail and 'write_flash' in args: raise ValueError('MCU write failed')
            def flash(*args):
                events.append('write_fpga')
                if fpga_fail: raise ValueError('USB disconnected')
            with patch.object(core,'device_ports',return_value=['test'] if ports is None else ports),patch.object(core,'validate_image'),patch.object(core,'version',side_effect=[ValueError('No console') if source is None else source,TARGET]),patch.object(core,'run_tool',side_effect=tool),patch.object(fpga,'FPGA_HASH', 'bad' if corrupt_fpga else hashlib.sha256(b'fpga').hexdigest()),patch.object(fpga,'preflight'),patch.object(fpga,'flash',side_effect=flash),patch.object(core.time,'sleep'):
                error=None
                try:core.install('test',image,folder,lambda s:None,f)
                except ValueError as exc:error=exc
                stage=json.loads((folder/'installation.json').read_text())['stage'] if (folder/'installation.json').exists() else None
            return events,error,stage
    def test_success_order(self):
        events,error,stage=self.perform()
        self.assertIsNone(error);self.assertEqual(stage,'complete')
        self.assertEqual(events,['read_flash','verify_flash','write_mcu','verify_flash','write_fpga'])
    def test_bad_backup_never_writes(self):
        for kwargs in [{'corrupt_backup':True},{'read_fail':True}]:
            events,error,_=self.perform(**kwargs);self.assertIsNotNone(error)
            self.assertNotIn('write_mcu',events);self.assertNotIn('write_fpga',events)
    def test_wrong_fpga_never_touches_device(self):
        events,error,_=self.perform(corrupt_fpga=True);self.assertIsNotNone(error);self.assertEqual(events,[])
    def test_partial_install_recorded(self):
        _,error,stage=self.perform(fpga_fail=True);self.assertIsNotNone(error);self.assertEqual(stage,'partial_install_fpga_failed')
    def test_unknown_source_rejected(self):
        events,error,_=self.perform(source={'mcu':'v9'});self.assertIsNotNone(error);self.assertEqual(events,[])
    def test_older_stock_identified_from_backup(self):
        _,error,stage=self.perform(source=None);self.assertIsNone(error);self.assertEqual(stage,'complete')
    def test_multiple_devices_rejected(self):
        events,error,_=self.perform(ports=['test','other']);self.assertIsNotNone(error);self.assertEqual(events,[])
    def test_mcu_failure_never_writes_fpga(self):
        events,error,stage=self.perform(mcu_fail=True)
        self.assertIsNotNone(error);self.assertNotIn('write_fpga',events);self.assertEqual(stage,'mcu_write_started')
    def test_interrupted_pair_can_retry(self):
        _,error,stage=self.perform(source=dict(TARGET,fpga='18.37'));self.assertIsNone(error);self.assertEqual(stage,'complete')
if __name__=='__main__':unittest.main()
