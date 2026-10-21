import struct
from dataclasses import dataclass

FIELDS = ('magic', 'cblp', 'cp', 'crlc', 'cparhdr', 'minalloc', 'maxalloc',
          'ss', 'sp', 'csum', 'ip', 'cs', 'lfarlc', 'ovno')


@dataclass(frozen=True)
class Image:
    header: dict
    payload: bytes
    relocations: tuple

    @property
    def memory_size(self):
        return ((len(self.payload) + 15) // 16 + self.header['minalloc']) * 16

    def load(self, paragraph):
        if paragraph < 0 or paragraph * 16 + self.memory_size > 0x100000:
            raise ValueError('load outside real-mode memory')
        memory = bytearray(b'\xa5' * self.memory_size)
        memory[:len(self.payload)] = self.payload
        for site in self.relocations:
            value, = struct.unpack_from('<H', memory, site)
            if value + paragraph > 0xffff:
                raise ValueError('segment overflow')
            struct.pack_into('<H', memory, site, value + paragraph)
        h = self.header
        return memory, (paragraph + h['cs'], h['ip']), (paragraph + h['ss'], h['sp'])


def parse(data):
    if len(data) < 28:
        raise ValueError('truncated MZ header')
    h = dict(zip(FIELDS, struct.unpack_from('<14H', data)))
    def require(condition, message):
        if not condition:
            raise ValueError(message)
    require(h['magic'] == 0x5a4d, 'bad magic')
    require(h['cp'] > 0 and h['cblp'] < 512, 'bad page fields')
    size = (h['cp'] - 1) * 512 + (h['cblp'] or 512)
    require(size == len(data), 'file length disagrees with header')
    header_size = h['cparhdr'] * 16
    table_end = 28 + 4 * h['crlc']
    require(h['lfarlc'] == 28, 'noncanonical relocation table offset')
    require(header_size == (table_end + 15) // 16 * 16 and header_size < size,
            'bad header size')
    require(data[table_end:header_size] == bytes(header_size - table_end), 'nonzero header padding')
    require(h['csum'] == 0 and h['ovno'] == 0, 'checksum or overlay unsupported')
    require(h['maxalloc'] == h['minalloc'], 'noncanonical allocation')
    payload = data[header_size:]
    require(len(payload) % 16 == 0, 'unrounded initialized image')
    memory_size = len(payload) + h['minalloc'] * 16
    require(h['cs'] == 0 and h['ip'] < h['ss'] * 16 <= min(65536, len(payload)), 'entry/code outside image')
    stack_top = h['ss'] * 16 + (h['sp'] or 65536)
    require(h['minalloc'] > 0 and h['sp'] % 2 == 0 and
            len(payload) < stack_top <= memory_size and
            memory_size - h['ss'] * 16 <= 65536, 'invalid stack/allocation')
    sites = []
    for index in range(h['crlc']):
        off, seg = struct.unpack_from('<HH', data, 28 + index * 4)
        site = seg * 16 + off
        require(off < 16 and site + 2 <= len(payload), 'relocation outside initialized image')
        require(not sites or site >= sites[-1] + 2, 'unordered or overlapping relocations')
        value, = struct.unpack_from('<H', payload, site)
        require(value in (0, h['ss']), 'unexpected relocation target segment')
        sites.append(site)
    return Image(h, payload, tuple(sites))
