#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reader for PPSSPP GE frame dumps (`.ppdmp`).

Format as written by PPSSPP v1.20.4 (`GPU/Debugger/Record.cpp`,
`RecordFormat.h`, read back by `Playback.cpp`):

  Header (24 bytes; 12 bytes before version 4):
    char magic[8] = "PPSSPPGE"; u32 version; char gameID[9]; u8 pad[3]
  u32 command_count
  u32 pushbuf_size
  compressed(commands)  # command_count packed records of 9 bytes:
                        #   u8 type; u32 sz; u32 ptr  (ptr/sz index pushbuf)
  compressed(pushbuf)   # pushbuf_size bytes
  where compressed(x) = u32 compressed_size + payload, snappy for versions
  2-4 and zstd for versions 5-6.

The file is untrusted: sizes are checked against the file and the expected
decompressed sizes, and every command must point inside the pushbuf. Nothing
is executed or written; only the parsed structure is returned.
"""

import dataclasses
import importlib
import importlib.util
import pathlib
import struct

MAGIC = b'PPSSPPGE'
MIN_VERSION = 2
MAX_VERSION = 6
COMMAND_SIZE = 9

# Hard caps so a hostile file cannot make us allocate without limit. Real
# dumps are a few MiB to a few hundred MiB of pushbuf.
DEFAULT_MAX_PUSHBUF = 1024 * 1024 * 1024
MAX_COMMANDS = 16 * 1024 * 1024

COMMAND_TYPES = {
    0x00: 'INIT',
    0x01: 'REGISTERS',
    0x02: 'VERTICES',
    0x03: 'INDICES',
    0x04: 'CLUT',
    0x05: 'TRANSFERSRC',
    0x06: 'MEMSET',
    0x07: 'MEMCPYDEST',
    0x08: 'MEMCPYDATA',
    0x09: 'DISPLAY',
    0x0A: 'CLUTADDR',
    0x0B: 'EDRAMTRANS',
}
for _level in range(8):
  COMMAND_TYPES[0x10 + _level] = f'TEXTURE{_level}'
  COMMAND_TYPES[0x18 + _level] = f'FRAMEBUF{_level}'
del _level


class DumpError(Exception):
  """Raised when a file is not a well-formed, supported `.ppdmp`."""


@dataclasses.dataclass(frozen=True)
class Command:
  type: int
  size: int
  ptr: int

  @property
  def name(self):
    return COMMAND_TYPES.get(self.type, f'UNKNOWN_{self.type:02X}')


@dataclasses.dataclass
class Dump:
  version: int
  game_id: str
  commands: list
  pushbuf: bytes
  compression: str
  file_size: int

  def data(self, command):
    """Returns the pushbuf bytes that `command` points to."""
    return self.pushbuf[command.ptr:command.ptr + command.size]


def _load_sibling(name):
  # Loaded by path so the tools also work under `python -I`, which drops the
  # script directory from sys.path.
  path = pathlib.Path(__file__).resolve().parent / f'{name}.py'
  spec = importlib.util.spec_from_file_location(f'mpo_tools_{name}', path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


zstd_lite = _load_sibling('zstd_lite')


def snappy_decompress(data, expected_size):
  """Decodes a raw snappy block (versions 2-4), capped at `expected_size`."""
  data = bytes(data)
  pos = 0
  length = 0
  shift = 0
  while True:
    if pos >= len(data) or shift > 28:
      raise DumpError('snappy: bad length varint')
    byte = data[pos]
    pos += 1
    length |= (byte & 0x7F) << shift
    shift += 7
    if not byte & 0x80:
      break
  if length != expected_size:
    raise DumpError(f'snappy: stream holds {length} bytes, expected '
                    f'{expected_size}')
  out = bytearray()
  while pos < len(data):
    tag = data[pos]
    pos += 1
    kind = tag & 3
    if kind == 0:
      size = (tag >> 2) + 1
      if size > 60:
        extra = size - 60
        if pos + extra > len(data):
          raise DumpError('snappy: literal length is truncated')
        size = int.from_bytes(data[pos:pos + extra], 'little') + 1
        pos += extra
      if pos + size > len(data) or len(out) + size > length:
        raise DumpError('snappy: literal is out of bounds')
      out += data[pos:pos + size]
      pos += size
      continue
    if kind == 1:
      if pos + 1 > len(data):
        raise DumpError('snappy: copy is truncated')
      size = ((tag >> 2) & 7) + 4
      offset = ((tag >> 5) << 8) | data[pos]
      pos += 1
    else:
      width = 2 if kind == 2 else 4
      if pos + width > len(data):
        raise DumpError('snappy: copy is truncated')
      size = (tag >> 2) + 1
      offset = int.from_bytes(data[pos:pos + width], 'little')
      pos += width
    if offset == 0 or offset > len(out) or len(out) + size > length:
      raise DumpError('snappy: copy is out of bounds')
    source = len(out) - offset
    if offset >= size:
      out += out[source:source + size]
    else:
      out += (bytes(out[source:]) * (size // offset + 1))[:size]
  if len(out) != length:
    raise DumpError('snappy: stream is truncated')
  return bytes(out)


def _fast_zstd_backend():
  """Returns a native zstd decompress(data, size) function, or None."""
  for name in ('compression.zstd', 'zstandard'):
    try:
      module = importlib.import_module(name)
    except ImportError:
      continue
    if name == 'compression.zstd':
      return lambda data, size: module.ZstdDecompressor().decompress(
          data, max_length=size)
    decompressor = module.ZstdDecompressor()
    return lambda data, size: decompressor.decompress(data,
                                                      max_output_size=size)
  return None


_FAST_ZSTD = _fast_zstd_backend()


def zstd_backend_name():
  return 'native' if _FAST_ZSTD is not None else 'pure-python'


def zstd_decompress(data, expected_size, force_pure=False):
  """Decompresses a zstd payload that must yield exactly `expected_size`."""
  if _FAST_ZSTD is not None and not force_pure:
    try:
      out = _FAST_ZSTD(bytes(data), expected_size)
    except Exception as error:  # pylint: disable=broad-except
      # Native backends raise their own error types; normalise them.
      raise DumpError(f'zstd: {error}') from error
  else:
    try:
      out = zstd_lite.decompress(data, max_output=expected_size)
    except zstd_lite.ZstdError as error:
      raise DumpError(f'zstd: {error}') from error
  if len(out) != expected_size:
    raise DumpError(f'zstd: payload holds {len(out)} bytes, expected '
                    f'{expected_size}')
  return out


def parse(raw, max_pushbuf=DEFAULT_MAX_PUSHBUF, force_pure=False):
  """Parses a whole `.ppdmp` file held in `raw` and returns a `Dump`."""
  raw = bytes(raw)
  if len(raw) < 12 or raw[:8] != MAGIC:
    raise DumpError('not a PPSSPP GE dump (bad magic)')
  (version,) = struct.unpack_from('<I', raw, 8)
  if not MIN_VERSION <= version <= MAX_VERSION:
    raise DumpError(f'unsupported dump version {version} (supported: '
                    f'{MIN_VERSION}-{MAX_VERSION})')
  pos = 12
  game_id = ''
  if version >= 4:
    if len(raw) < 24:
      raise DumpError('header is truncated')
    game_id = raw[12:21].split(b'\0', 1)[0].decode('ascii', errors='replace')
    pos = 24
  if pos + 8 > len(raw):
    raise DumpError('command and buffer counts are truncated')
  command_count, pushbuf_size = struct.unpack_from('<II', raw, pos)
  pos += 8
  if command_count > MAX_COMMANDS:
    raise DumpError(f'too many commands ({command_count})')
  if pushbuf_size > max_pushbuf:
    raise DumpError(f'pushbuf of {pushbuf_size} bytes exceeds the limit of '
                    f'{max_pushbuf}')

  compression = 'snappy' if version < 5 else 'zstd'
  blobs = []
  for label, expected in (('commands', command_count * COMMAND_SIZE),
                          ('pushbuf', pushbuf_size)):
    if pos + 4 > len(raw):
      raise DumpError(f'{label}: compressed size is truncated')
    (compressed_size,) = struct.unpack_from('<I', raw, pos)
    pos += 4
    if compressed_size > len(raw) - pos:
      raise DumpError(f'{label}: compressed data is truncated')
    payload = raw[pos:pos + compressed_size]
    pos += compressed_size
    try:
      if compression == 'snappy':
        blobs.append(snappy_decompress(payload, expected))
      else:
        blobs.append(zstd_decompress(payload, expected, force_pure))
    except DumpError as error:
      raise DumpError(f'{label}: {error}') from error
  if pos != len(raw):
    raise DumpError(f'{len(raw) - pos} unexpected trailing bytes')

  table, pushbuf = blobs
  commands = []
  for index in range(command_count):
    kind, size, ptr = struct.unpack_from('<BII', table, index * COMMAND_SIZE)
    if kind not in COMMAND_TYPES:
      raise DumpError(f'command {index}: unknown type 0x{kind:02X}')
    if ptr + size > pushbuf_size:
      raise DumpError(f'command {index} ({COMMAND_TYPES[kind]}): data at '
                      f'0x{ptr:X}+0x{size:X} is outside the pushbuf')
    commands.append(Command(kind, size, ptr))
  return Dump(version=version, game_id=game_id, commands=commands,
              pushbuf=pushbuf, compression=compression, file_size=len(raw))


def load(path, max_pushbuf=DEFAULT_MAX_PUSHBUF, force_pure=False):
  """Reads and parses the `.ppdmp` at `path`."""
  path = pathlib.Path(path)
  size = path.stat().st_size
  # The pushbuf cap bounds the file too: zstd/snappy never expand data by
  # more than a small constant factor, so a larger file is not a real dump.
  if size > max_pushbuf * 2 + 4096:
    raise DumpError(f'file of {size} bytes is too large')
  return parse(path.read_bytes(), max_pushbuf=max_pushbuf,
               force_pure=force_pure)
