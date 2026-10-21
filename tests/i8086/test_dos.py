#!/usr/bin/env python3
import struct
import tempfile
import unittest
from pathlib import Path

import dos
import mz
import run as bare

HERE = Path(__file__).resolve().parent


class Formats(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='dcc-dos-tests-')
        cls.root = Path(cls.directory.name)
        cls.com = cls.reference('reference_com')
        cls.exe = cls.reference('reference')

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    @classmethod
    def reference(cls, name, **defines):
        return dos.assemble(HERE / 'programs/dos' / (name + '.asm'),
                            cls.root / (name + '.bin'), defines, 'nasm')

    def test_reference_fields_and_load_simulation(self):
        image = mz.parse(self.exe)
        self.assertEqual(image.header, dict(zip(mz.FIELDS,
            (0x5a4d, 208, 1, 2, 3, 17, 17, 9, 288, 0, 0, 0, 28, 0))))
        self.assertEqual(image.relocations, (41, 153))
        for paragraph in (0x2000, 0x3456, 0x7000):
            memory, entry, stack = image.load(paragraph)
            self.assertEqual(entry, (paragraph, 0))
            self.assertEqual(stack, (paragraph + 9, 288))
            expected = bytearray(image.payload)
            for site in (41, 153):
                struct.pack_into('<H', expected, site, paragraph + 9)
            self.assertEqual(memory[:160], expected)
            self.assertEqual(memory[160:], b'\xa5' * 272)
            offset, segment = struct.unpack_from('<HH', memory, 151)
            self.assertEqual(segment * 16 + offset, paragraph * 16 + 144)

    def test_all_header_fields_against_reference(self):
        expected = mz.parse(self.exe).header
        for index, name in enumerate(mz.FIELDS):
            data = bytearray(self.exe)
            struct.pack_into('<H', data, index * 2, expected[name] ^ 1)
            with self.subTest(field=name):
                try:
                    changed = mz.parse(data)
                except ValueError:
                    continue
                self.assertNotEqual(changed.header, expected)

    def test_malformed_images(self):
        cases = [b'', self.exe[:27], self.exe[:-1], self.exe + b'\0']
        for offset, value in ((2, 512), (4, 0), (6, 0xffff), (8, 1),
                              (10, 0), (12, 0xffff), (14, 0xffff), (16, 1),
                              (18, 1), (20, 0xffff), (22, 1), (24, 30), (26, 1),
                              (28, 16), (30, 0xffff), (32, 16), (34, 2)):
            data = bytearray(self.exe)
            struct.pack_into('<H', data, offset, value)
            cases.append(data)
        data = bytearray(self.exe)
        data[36] = 1
        cases.append(data)
        data = bytearray(self.exe)
        data[32:36] = data[28:32]
        cases.append(data)
        data = bytearray(self.exe)
        struct.pack_into('<H', data, 48 + 41, 7)
        cases.append(data)
        for data in cases:
            with self.subTest(data=bytes(data[:36]).hex(), size=len(data)):
                with self.assertRaises(ValueError):
                    mz.parse(data)

    def test_page_boundary_and_zero_relocations(self):
        payload = bytearray(480)
        payload[:5] = b'\xb8\x35\x4c\xcd\x21'
        header = struct.pack('<14H', 0x5a4d, 0, 1, 0, 2, 1, 1,
                             1, 480, 0, 0, 0, 28, 0) + bytes(4)
        image = header + payload
        parsed = mz.parse(image)
        self.assertEqual(parsed.relocations, ())
        self.assertEqual(parsed.memory_size, 496)
        for size in (511, 513):
            with self.assertRaises(ValueError):
                mz.parse((image + b'\0')[:size])
        result = dos.run(image, 'mz')
        self.assertIsNone(result.error)
        self.assertEqual(result.exit_code, 53)

    def test_bss_stack_not_stored(self):
        larger = self.reference('reference', STACK_SIZE=4096)
        self.assertEqual(len(larger), len(self.exe))
        self.assertEqual(mz.parse(larger).header['minalloc'], 257)
        self.assertEqual(mz.parse(larger).header['sp'], 4128)

    def test_execution_load_matrix_and_full_exit_range(self):
        for format, name, stdout in (('com', 'reference_com', b'CO\0\x80\r\n'),
                                      ('mz', 'reference', b'MZ\0\x80\r\n')):
            for paragraph, code in ((0x2000, 0), (0x3456, 127), (0x5000, 128),
                                    (0x7000, 254), (0x3000, 255)):
                with self.subTest(format=format, paragraph=paragraph, code=code):
                    binary = self.reference(name, EXIT_CODE=code)
                    result = dos.run(binary, format, paragraph, b' arg')
                    self.assertIsNone(result.error)
                    self.assertFalse(result.timed_out)
                    self.assertEqual(result.exit_code, code)
                    self.assertEqual(result.stdout, stdout)
                    self.assertEqual(result.stderr, stdout[:2])

    def test_harness_failure_and_timeout(self):
        result = dos.run(b'\xb4\x30\xcd\x21', 'com')
        self.assertIn('unsupported DOS service AH=30', result.error)
        self.assertIsNone(result.exit_code)
        result = dos.run(b'\xeb\xfe', 'com', timeout=0.5)
        self.assertTrue(result.timed_out)
        self.assertIsNone(result.exit_code)
        for serial, status in ((b'', 1), (b'\3', 1), (b'\5\0', 1),
                               (b'\3\0\1\0', 1), (b'\3\0', 0)):
            self.assertIsNotNone(dos.decode(bare.Result(0, serial, False, status)).error)
        result = dos.run(b'\x30\xc0\xe6\xf4', 'com')
        self.assertIn('missing DOS completion', result.error)

    def test_large_image_and_zero_stack_pointer(self):
        payload = bytearray(0x1fff0)
        payload[:5] = b'\xb8\x63\x4c\xcd\x21'
        size = 32 + len(payload)
        header = struct.pack('<14H', 0x5a4d, size % 512, (size + 511)//512,
                             0, 2, 1, 1, 0x1000, 0, 0, 0, 0, 28, 0) + bytes(4)
        binary = header + payload
        self.assertEqual(mz.parse(binary).memory_size, 0x20000)
        result = dos.run(binary, 'mz', 0x5000)
        self.assertIsNone(result.error)
        self.assertEqual(result.exit_code, 99)
        with self.assertRaises(bare.HarnessError):
            dos.build_image(binary, 'mz', 0x2000)

    def test_cross_paragraph_relocation_and_zero_segment(self):
        binary = self.reference('boundary')
        image = mz.parse(binary)
        self.assertEqual(image.relocations, (15, 31))
        loaded, _, _ = image.load(0x3456)
        self.assertEqual(struct.unpack_from('<H', loaded, 15)[0], 0x3456)
        self.assertEqual(struct.unpack_from('<H', loaded, 31)[0], 0x345c)
        result = dos.run(binary, 'mz', 0x3456)
        self.assertIsNone(result.error)
        self.assertEqual(result.exit_code, 7)

    def test_input_rejection(self):
        for binary, format, paragraph, tail in ((b'', 'com', 0x3000, b''),
                (b'\0' * 0xef01, 'com', 0x3000, b''), (self.exe, 'mz', 0x1000, b''),
                (self.exe, 'mz', 0x9000, b''), (self.exe, 'flat', 0x3000, b''),
                (self.com, 'com', 0x3000, b'x' * 127), (self.exe, 'mz', 0x3000, b'\r')):
            with self.assertRaises(bare.HarnessError):
                dos.build_image(binary, format, paragraph, tail)


if __name__ == '__main__':
    unittest.main(verbosity=2)
