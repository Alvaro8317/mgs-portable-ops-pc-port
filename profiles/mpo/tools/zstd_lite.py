#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure-Python Zstandard decoder (RFC 8878) and XXH64.

PPSSPP writes its GE frame dumps (version 5 and later) with zstd. This module
lets the tools read them with nothing but the standard library, so they run
under `python -I` and in CI. It is slow (about 1-3 MB/s); `ppdmp.py` uses the
`zstandard` package or `compression.zstd` (Python 3.14+) instead when one of
them can be imported.

Only what PPSSPP needs is supported: zstd frames without a dictionary, plus
skippable frames. The input is untrusted: every size is checked, the output is
bounded by `max_output`, and any malformed input raises `ZstdError` (never an
IndexError or a MemoryError).

Written from the format description in RFC 8878; no code was copied.
"""

import struct

_FRAME_MAGIC = 0xFD2FB528
_SKIPPABLE_MAGIC_MASK = 0xFFFFFFF0
_SKIPPABLE_MAGIC = 0x184D2A50
_BLOCK_SIZE_MAX = 128 * 1024
_WINDOW_LOG_MAX = 31

_LL_BASELINES = (list(range(16)) + [
    16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096,
    8192, 16384, 32768, 65536
])
_LL_EXTRA_BITS = ([0] * 16 +
                  [1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14,
                   15, 16])
_ML_BASELINES = ([code + 3 for code in range(32)] + [
    35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051,
    4099, 8195, 16387, 32771, 65539
])
_ML_EXTRA_BITS = ([0] * 32 +
                  [1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13,
                   14, 15, 16])

_LL_DEFAULT = (4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2,
               2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1)
_ML_DEFAULT = (1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
               1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
               1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1)
_OF_DEFAULT = (1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
               1, 1, 1, -1, -1, -1, -1, -1)

# (max symbol, max accuracy log) per sequence table kind.
_LL_LIMITS = (35, 9)
_ML_LIMITS = (52, 9)
_OF_LIMITS = (31, 8)
_HUF_WEIGHT_ACCURACY_MAX = 6
_HUF_MAX_BITS = 11

_MASK64 = (1 << 64) - 1
_P64_1 = 0x9E3779B185EBCA87
_P64_2 = 0xC2B2AE3D27D4EB4F
_P64_3 = 0x165667B19E3779F9
_P64_4 = 0x85EBCA77C2B2AE63
_P64_5 = 0x27D4EB2F165667C5


class ZstdError(Exception):
  """Raised when the input is not a valid (supported) zstd stream."""


def _rotl64(value, bits):
  return ((value << bits) | (value >> (64 - bits))) & _MASK64


def _xxh64_round(acc, lane):
  acc = (acc + lane * _P64_2) & _MASK64
  return (_rotl64(acc, 31) * _P64_1) & _MASK64


def _xxh64_merge(acc, value):
  acc ^= _xxh64_round(0, value)
  return (acc * _P64_1 + _P64_4) & _MASK64


def xxh64(data, seed=0):
  """Returns the 64-bit XXH64 hash of `data`."""
  data = memoryview(data).cast('B')
  length = len(data)
  pos = 0
  if length >= 32:
    v1 = (seed + _P64_1 + _P64_2) & _MASK64
    v2 = (seed + _P64_2) & _MASK64
    v3 = seed & _MASK64
    v4 = (seed - _P64_1) & _MASK64
    stripes = length // 32
    lanes = struct.unpack_from(f'<{stripes * 4}Q', data, 0)
    for index in range(0, stripes * 4, 4):
      v1 = _xxh64_round(v1, lanes[index])
      v2 = _xxh64_round(v2, lanes[index + 1])
      v3 = _xxh64_round(v3, lanes[index + 2])
      v4 = _xxh64_round(v4, lanes[index + 3])
    pos = stripes * 32
    acc = (_rotl64(v1, 1) + _rotl64(v2, 7) + _rotl64(v3, 12) +
           _rotl64(v4, 18)) & _MASK64
    for value in (v1, v2, v3, v4):
      acc = _xxh64_merge(acc, value)
  else:
    acc = (seed + _P64_5) & _MASK64
  acc = (acc + length) & _MASK64
  while pos + 8 <= length:
    (lane,) = struct.unpack_from('<Q', data, pos)
    acc ^= _xxh64_round(0, lane)
    acc = (_rotl64(acc, 27) * _P64_1 + _P64_4) & _MASK64
    pos += 8
  if pos + 4 <= length:
    (lane,) = struct.unpack_from('<I', data, pos)
    acc ^= (lane * _P64_1) & _MASK64
    acc = (_rotl64(acc, 23) * _P64_2 + _P64_3) & _MASK64
    pos += 4
  while pos < length:
    acc ^= (data[pos] * _P64_5) & _MASK64
    acc = (_rotl64(acc, 11) * _P64_1) & _MASK64
    pos += 1
  acc ^= acc >> 33
  acc = (acc * _P64_2) & _MASK64
  acc ^= acc >> 29
  acc = (acc * _P64_3) & _MASK64
  acc ^= acc >> 32
  return acc


class _ForwardBits:
  """Little-endian, LSB-first bit reader (used for FSE table headers)."""

  def __init__(self, data, start, end):
    self._data = data
    self._bit = start * 8
    self._end_bit = end * 8

  def read(self, count):
    if count == 0:
      return 0
    if self._bit + count > self._end_bit:
      raise ZstdError('FSE table description is truncated')
    first = self._bit >> 3
    last = (self._bit + count + 7) >> 3
    value = int.from_bytes(self._data[first:last], 'little')
    value = (value >> (self._bit & 7)) & ((1 << count) - 1)
    self._bit += count
    return value

  def rewind(self, count):
    self._bit -= count

  def aligned_offset(self):
    """Returns the byte offset after rounding up to a whole byte."""
    return (self._bit + 7) >> 3


class _BackwardBits:
  """Reader for zstd's backward bitstreams (read from the end, MSB first).

  `pos` is the number of unread bits. Reading past the start yields zeros and
  makes `pos` negative, which callers use to detect overflow or the end.
  """

  def __init__(self, data, start, end):
    if end <= start:
      raise ZstdError('empty bitstream')
    last = data[end - 1]
    if last == 0:
      raise ZstdError('bitstream has no end marker')
    self._data = data
    self._start = start
    self.pos = (end - start) * 8 - (8 - (last.bit_length() - 1))

  def read(self, count):
    if count == 0:
      return 0
    end = self.pos
    begin = end - count
    self.pos = begin
    if end <= 0:
      return 0
    mask = (1 << count) - 1
    if begin >= 0:
      first = self._start + (begin >> 3)
      last = self._start + ((end + 7) >> 3)
      value = int.from_bytes(self._data[first:last], 'little')
      return (value >> (begin & 7)) & mask
    value = int.from_bytes(self._data[self._start:self._start +
                                      ((end + 7) >> 3)], 'little')
    value &= (1 << end) - 1
    return (value << -begin) & mask


class _FseTable:
  """FSE decoding table: one (symbol, bits, base) triple per state."""

  def __init__(self, accuracy_log, symbols, num_bits, bases):
    self.accuracy_log = accuracy_log
    self.symbols = symbols
    self.num_bits = num_bits
    self.bases = bases

  @classmethod
  def from_distribution(cls, distribution, accuracy_log):
    size = 1 << accuracy_log
    symbols = [0] * size
    next_state = [0] * len(distribution)
    high = size
    for symbol, count in enumerate(distribution):
      if count == -1:
        high -= 1
        symbols[high] = symbol
        next_state[symbol] = 1
    step = (size >> 1) + (size >> 3) + 3
    mask = size - 1
    position = 0
    for symbol, count in enumerate(distribution):
      if count <= 0:
        continue
      next_state[symbol] = count
      for _ in range(count):
        symbols[position] = symbol
        position = (position + step) & mask
        while position >= high:
          position = (position + step) & mask
    if position != 0:
      raise ZstdError('invalid FSE distribution')
    num_bits = [0] * size
    bases = [0] * size
    for state in range(size):
      symbol = symbols[state]
      descriptor = next_state[symbol]
      next_state[symbol] += 1
      bits = accuracy_log - (descriptor.bit_length() - 1)
      num_bits[state] = bits
      bases[state] = (descriptor << bits) - size
    return cls(accuracy_log, symbols, num_bits, bases)

  @classmethod
  def rle(cls, symbol):
    return cls(0, [symbol], [0], [0])


def _read_fse_distribution(data, start, end, max_symbol, max_accuracy):
  """Parses an FSE table description; returns (table, bytes consumed)."""
  bits = _ForwardBits(data, start, end)
  accuracy_log = bits.read(4) + 5
  if accuracy_log > max_accuracy:
    raise ZstdError(f'FSE accuracy log {accuracy_log} is too large')
  remaining = 1 << accuracy_log
  distribution = []
  while remaining > 0:
    if len(distribution) > max_symbol:
      raise ZstdError('FSE table has too many symbols')
    width = (remaining + 1).bit_length()
    value = bits.read(width)
    lower_mask = (1 << (width - 1)) - 1
    threshold = (1 << width) - 1 - (remaining + 1)
    if (value & lower_mask) < threshold:
      bits.rewind(1)
      value &= lower_mask
    elif value > lower_mask:
      value -= threshold
    probability = value - 1
    remaining -= -probability if probability < 0 else probability
    distribution.append(probability)
    if probability == 0:
      while True:
        repeat = bits.read(2)
        distribution.extend([0] * repeat)
        if repeat != 3:
          break
      if len(distribution) > max_symbol + 1:
        raise ZstdError('FSE table has too many symbols')
  if remaining != 0:
    raise ZstdError('FSE probabilities do not add up')
  table = _FseTable.from_distribution(distribution, accuracy_log)
  return table, bits.aligned_offset() - start


class _HuffmanTable:
  """Huffman decoding table indexed by `max_bits` bits of state."""

  def __init__(self, weights):
    total = 0
    for weight in weights:
      if weight > _HUF_MAX_BITS:
        raise ZstdError('Huffman weight is too large')
      if weight:
        total += 1 << (weight - 1)
    if total == 0:
      raise ZstdError('Huffman table has no symbols')
    max_bits = total.bit_length()
    rest = (1 << max_bits) - total
    if rest & (rest - 1):
      raise ZstdError('Huffman weights do not form a complete tree')
    weights = list(weights) + [rest.bit_length()]
    if max_bits > _HUF_MAX_BITS or len(weights) > 256:
      raise ZstdError('Huffman table is too large')
    lengths = [max_bits + 1 - weight if weight else 0 for weight in weights]
    rank_count = [0] * (max_bits + 1)
    for length in lengths:
      if length:
        rank_count[length] += 1
    size = 1 << max_bits
    symbols = bytearray(size)
    num_bits = bytearray(size)
    rank_start = [0] * (max_bits + 2)
    for length in range(max_bits, 0, -1):
      rank_start[length - 1] = (rank_start[length] +
                                rank_count[length] * (1 << (max_bits - length)))
      for index in range(rank_start[length], rank_start[length - 1]):
        num_bits[index] = length
    for symbol, length in enumerate(lengths):
      if length:
        code = rank_start[length]
        span = 1 << (max_bits - length)
        symbols[code:code + span] = bytes([symbol]) * span
        rank_start[length] += span
    self.max_bits = max_bits
    self.symbols = symbols
    self.num_bits = num_bits


def _read_huffman_table(data, start, end):
  """Parses a Huffman tree description; returns (table, bytes consumed)."""
  if start >= end:
    raise ZstdError('Huffman tree description is truncated')
  header = data[start]
  if header >= 128:
    count = header - 127
    size = (count + 1) // 2
    if start + 1 + size > end:
      raise ZstdError('Huffman weights are truncated')
    weights = []
    for index in range(count):
      byte = data[start + 1 + index // 2]
      weights.append(byte >> 4 if index % 2 == 0 else byte & 0xF)
    return _HuffmanTable(weights), 1 + size
  size = header
  if size == 0 or start + 1 + size > end:
    raise ZstdError('Huffman weights are truncated')
  table, consumed = _read_fse_distribution(data, start + 1, start + 1 + size,
                                           255, _HUF_WEIGHT_ACCURACY_MAX)
  stream = _BackwardBits(data, start + 1 + consumed, start + 1 + size)
  accuracy = table.accuracy_log
  symbols, num_bits, bases = table.symbols, table.num_bits, table.bases
  state1 = stream.read(accuracy)
  state2 = stream.read(accuracy)
  weights = []
  while True:
    if len(weights) > 255:
      raise ZstdError('too many Huffman weights')
    weights.append(symbols[state1])
    state1 = bases[state1] + stream.read(num_bits[state1])
    if stream.pos < 0:
      weights.append(symbols[state2])
      break
    weights.append(symbols[state2])
    state2 = bases[state2] + stream.read(num_bits[state2])
    if stream.pos < 0:
      weights.append(symbols[state1])
      break
  if len(weights) > 255:
    raise ZstdError('too many Huffman weights')
  return _HuffmanTable(weights), 1 + size


def _decode_huffman_stream(table, data, start, end, count, out):
  """Decodes `count` literals from one backward stream into `out`."""
  stream = _BackwardBits(data, start, end)
  max_bits = table.max_bits
  mask = (1 << max_bits) - 1
  symbols, num_bits = table.symbols, table.num_bits
  state = stream.read(max_bits)
  read = stream.read
  append = out.append
  for _ in range(count):
    append(symbols[state])
    bits = num_bits[state]
    state = ((state << bits) | read(bits)) & mask
  if stream.pos != -max_bits:
    raise ZstdError('Huffman stream size mismatch')


class _FrameState:
  """State carried between the blocks of one frame."""

  def __init__(self, window_size, frame_start):
    self.window_size = window_size
    self.frame_start = frame_start
    self.huffman = None
    self.tables = {'ll': None, 'of': None, 'ml': None}
    self.repeat_offsets = [1, 4, 8]


_DEFAULT_TABLES = {
    'll': (_LL_DEFAULT, 6),
    'of': (_OF_DEFAULT, 5),
    'ml': (_ML_DEFAULT, 6),
}
_PREDEFINED = {}


def _predefined_table(kind):
  if kind not in _PREDEFINED:
    distribution, accuracy = _DEFAULT_TABLES[kind]
    _PREDEFINED[kind] = _FseTable.from_distribution(distribution, accuracy)
  return _PREDEFINED[kind]


def _read_literals(data, start, end, state):
  """Decodes the literals section; returns (literals, bytes consumed)."""
  if start >= end:
    raise ZstdError('literals section is missing')
  byte0 = data[start]
  block_type = byte0 & 3
  size_format = (byte0 >> 2) & 3
  if block_type in (0, 1):
    if size_format in (0, 2):
      header_size, regenerated = 1, byte0 >> 3
    elif size_format == 1:
      header_size = 2
      if start + 2 > end:
        raise ZstdError('literals header is truncated')
      regenerated = (byte0 >> 4) | (data[start + 1] << 4)
    else:
      header_size = 3
      if start + 3 > end:
        raise ZstdError('literals header is truncated')
      regenerated = ((byte0 >> 4) | (data[start + 1] << 4) |
                     (data[start + 2] << 12))
    pos = start + header_size
    if regenerated > _BLOCK_SIZE_MAX:
      raise ZstdError('literals section is too large')
    if block_type == 0:
      if pos + regenerated > end:
        raise ZstdError('raw literals are truncated')
      return bytes(data[pos:pos + regenerated]), header_size + regenerated
    if pos + 1 > end:
      raise ZstdError('RLE literals are truncated')
    return bytes([data[pos]]) * regenerated, header_size + 1

  header_size, field_bits = ((3, 10), (3, 10), (4, 14), (5, 18))[size_format]
  streams = 1 if size_format == 0 else 4
  if start + header_size > end:
    raise ZstdError('literals header is truncated')
  header = int.from_bytes(data[start:start + header_size], 'little')
  field_mask = (1 << field_bits) - 1
  regenerated = (header >> 4) & field_mask
  compressed = (header >> (4 + field_bits)) & field_mask
  if regenerated > _BLOCK_SIZE_MAX:
    raise ZstdError('literals section is too large')
  pos = start + header_size
  stop = pos + compressed
  if stop > end:
    raise ZstdError('compressed literals are truncated')
  if block_type == 2:
    state.huffman, consumed = _read_huffman_table(data, pos, stop)
    pos += consumed
  elif state.huffman is None:
    raise ZstdError('treeless literals without a previous Huffman table')
  out = bytearray()
  if streams == 1:
    _decode_huffman_stream(state.huffman, data, pos, stop, regenerated, out)
  else:
    if pos + 6 > stop:
      raise ZstdError('literals jump table is truncated')
    sizes = list(struct.unpack_from('<3H', data, pos))
    pos += 6
    sizes.append(stop - pos - sum(sizes))
    if sizes[3] < 0:
      raise ZstdError('literals jump table is out of bounds')
    chunk = (regenerated + 3) // 4
    counts = [chunk, chunk, chunk, regenerated - 3 * chunk]
    if counts[3] < 0:
      raise ZstdError('too few literals for four streams')
    for size, count in zip(sizes, counts):
      _decode_huffman_stream(state.huffman, data, pos, pos + size, count, out)
      pos += size
  return bytes(out), header_size + compressed


def _read_sequence_table(kind, mode, data, pos, end, state):
  """Selects or parses one sequence decoding table; returns bytes used."""
  max_symbol, max_accuracy = {
      'll': _LL_LIMITS,
      'of': _OF_LIMITS,
      'ml': _ML_LIMITS
  }[kind]
  if mode == 0:
    state.tables[kind] = _predefined_table(kind)
    return 0
  if mode == 1:
    if pos >= end:
      raise ZstdError('RLE sequence table is truncated')
    symbol = data[pos]
    if symbol > max_symbol:
      raise ZstdError('RLE sequence symbol is out of range')
    state.tables[kind] = _FseTable.rle(symbol)
    return 1
  if mode == 2:
    table, consumed = _read_fse_distribution(data, pos, end, max_symbol,
                                             max_accuracy)
    state.tables[kind] = table
    return consumed
  if state.tables[kind] is None:
    raise ZstdError('repeat mode without a previous sequence table')
  return 0


def _decode_block(data, start, end, state, out, max_output):
  """Decodes one compressed block, appending to `out`."""
  literals, consumed = _read_literals(data, start, end, state)
  pos = start + consumed
  if pos >= end:
    raise ZstdError('sequences section is missing')
  byte0 = data[pos]
  if byte0 == 0:
    count, pos = 0, pos + 1
  elif byte0 < 128:
    count, pos = byte0, pos + 1
  elif byte0 < 255:
    if pos + 2 > end:
      raise ZstdError('sequence count is truncated')
    count, pos = ((byte0 - 128) << 8) + data[pos + 1], pos + 2
  else:
    if pos + 3 > end:
      raise ZstdError('sequence count is truncated')
    count, pos = data[pos + 1] + (data[pos + 2] << 8) + 0x7F00, pos + 3

  if count == 0:
    if pos != end:
      raise ZstdError('trailing bytes after an empty sequences section')
    if len(out) + len(literals) > max_output:
      raise ZstdError('output exceeds the expected size')
    out += literals
    return

  if pos >= end:
    raise ZstdError('symbol compression modes are missing')
  modes = data[pos]
  pos += 1
  if modes & 3:
    raise ZstdError('reserved bits set in symbol compression modes')
  pos += _read_sequence_table('ll', modes >> 6, data, pos, end, state)
  pos += _read_sequence_table('of', (modes >> 4) & 3, data, pos, end, state)
  pos += _read_sequence_table('ml', (modes >> 2) & 3, data, pos, end, state)
  if pos > end:
    raise ZstdError('sequence tables overrun the block')

  stream = _BackwardBits(data, pos, end)
  read = stream.read
  ll_table, of_table, ml_table = (state.tables['ll'], state.tables['of'],
                                  state.tables['ml'])
  ll_symbols, ll_bits, ll_bases = (ll_table.symbols, ll_table.num_bits,
                                   ll_table.bases)
  of_symbols, of_bits, of_bases = (of_table.symbols, of_table.num_bits,
                                   of_table.bases)
  ml_symbols, ml_bits, ml_bases = (ml_table.symbols, ml_table.num_bits,
                                   ml_table.bases)
  ll_state = read(ll_table.accuracy_log)
  of_state = read(of_table.accuracy_log)
  ml_state = read(ml_table.accuracy_log)
  reps = state.repeat_offsets
  literal_pos = 0
  literal_count = len(literals)
  for index in range(count):
    of_code = of_symbols[of_state]
    ll_code = ll_symbols[ll_state]
    ml_code = ml_symbols[ml_state]
    if of_code > _WINDOW_LOG_MAX:
      raise ZstdError('offset code is out of range')
    offset_value = (1 << of_code) + read(of_code)
    match_length = _ML_BASELINES[ml_code] + read(_ML_EXTRA_BITS[ml_code])
    literal_length = _LL_BASELINES[ll_code] + read(_LL_EXTRA_BITS[ll_code])
    if index + 1 < count:
      ll_state = ll_bases[ll_state] + read(ll_bits[ll_state])
      ml_state = ml_bases[ml_state] + read(ml_bits[ml_state])
      of_state = of_bases[of_state] + read(of_bits[of_state])

    if offset_value > 3:
      offset = offset_value - 3
      reps[2], reps[1], reps[0] = reps[1], reps[0], offset
    else:
      repeat = offset_value - 1 + (1 if literal_length == 0 else 0)
      if repeat == 0:
        offset = reps[0]
      elif repeat == 1:
        offset = reps[1]
        reps[1], reps[0] = reps[0], offset
      elif repeat == 2:
        offset = reps[2]
        reps[2], reps[1], reps[0] = reps[1], reps[0], offset
      else:
        offset = reps[0] - 1
        reps[2], reps[1], reps[0] = reps[1], reps[0], offset

    if literal_pos + literal_length > literal_count:
      raise ZstdError('sequence uses more literals than decoded')
    if len(out) + literal_length + match_length > max_output:
      raise ZstdError('output exceeds the expected size')
    out += literals[literal_pos:literal_pos + literal_length]
    literal_pos += literal_length
    if (offset <= 0 or offset > len(out) - state.frame_start or
        offset > state.window_size):
      raise ZstdError('match offset is out of range')
    source = len(out) - offset
    if offset >= match_length:
      out += out[source:source + match_length]
    else:
      pattern = bytes(out[source:])
      repeats = match_length // offset + 1
      out += (pattern * repeats)[:match_length]

  if stream.pos != 0:
    raise ZstdError('sequence bitstream size mismatch')
  if len(out) + literal_count - literal_pos > max_output:
    raise ZstdError('output exceeds the expected size')
  out += literals[literal_pos:]


def _decode_frame(data, pos, out, max_output):
  """Decodes one zstd frame at `pos`; returns the position after it."""
  end = len(data)
  if pos + 1 > end:
    raise ZstdError('frame header is truncated')
  descriptor = data[pos]
  pos += 1
  size_flag = descriptor >> 6
  single_segment = (descriptor >> 5) & 1
  if descriptor & 0x08:
    raise ZstdError('reserved bit set in frame header')
  has_checksum = (descriptor >> 2) & 1
  dict_id_size = (0, 1, 2, 4)[descriptor & 3]
  window_size = None
  if not single_segment:
    if pos + 1 > end:
      raise ZstdError('window descriptor is truncated')
    window_byte = data[pos]
    pos += 1
    exponent = window_byte >> 3
    if exponent + 10 > _WINDOW_LOG_MAX:
      raise ZstdError('window is too large')
    window_base = 1 << (10 + exponent)
    window_size = window_base + (window_base // 8) * (window_byte & 7)
  if pos + dict_id_size > end:
    raise ZstdError('dictionary ID is truncated')
  if dict_id_size and int.from_bytes(data[pos:pos + dict_id_size], 'little'):
    raise ZstdError('zstd dictionaries are not supported')
  pos += dict_id_size
  content_size_bytes = (1 if single_segment else 0, 2, 4, 8)[size_flag]
  content_size = None
  if content_size_bytes:
    if pos + content_size_bytes > end:
      raise ZstdError('frame content size is truncated')
    content_size = int.from_bytes(data[pos:pos + content_size_bytes],
                                  'little')
    if content_size_bytes == 2:
      content_size += 256
    pos += content_size_bytes
  if window_size is None:
    window_size = content_size
  frame_start = len(out)
  if content_size is not None and frame_start + content_size > max_output:
    raise ZstdError('frame content size exceeds the expected size')

  state = _FrameState(window_size, frame_start)
  block_limit = min(window_size, _BLOCK_SIZE_MAX)
  while True:
    if pos + 3 > end:
      raise ZstdError('block header is truncated')
    header = int.from_bytes(data[pos:pos + 3], 'little')
    pos += 3
    last = header & 1
    block_type = (header >> 1) & 3
    block_size = header >> 3
    if block_type == 3:
      raise ZstdError('reserved block type')
    if block_size > block_limit:
      raise ZstdError('block is larger than allowed')
    if block_type == 0:
      if pos + block_size > end:
        raise ZstdError('raw block is truncated')
      if len(out) + block_size > max_output:
        raise ZstdError('output exceeds the expected size')
      out += data[pos:pos + block_size]
      pos += block_size
    elif block_type == 1:
      if pos + 1 > end:
        raise ZstdError('RLE block is truncated')
      if len(out) + block_size > max_output:
        raise ZstdError('output exceeds the expected size')
      out += bytes([data[pos]]) * block_size
      pos += 1
    else:
      if pos + block_size > end:
        raise ZstdError('compressed block is truncated')
      before = len(out)
      _decode_block(data, pos, pos + block_size, state, out, max_output)
      if len(out) - before > _BLOCK_SIZE_MAX:
        raise ZstdError('block decompresses to more than 128 KiB')
      pos += block_size
    if last:
      break

  if content_size is not None and len(out) - frame_start != content_size:
    raise ZstdError('frame content size mismatch')
  if has_checksum:
    if pos + 4 > end:
      raise ZstdError('content checksum is truncated')
    (expected,) = struct.unpack_from('<I', data, pos)
    pos += 4
    actual = xxh64(memoryview(out)[frame_start:]) & 0xFFFFFFFF
    if actual != expected:
      raise ZstdError('content checksum mismatch')
  return pos


def decompress(data, max_output):
  """Decompresses all frames in `data`; output is capped at `max_output`."""
  data = bytes(data)
  out = bytearray()
  pos = 0
  if not data:
    raise ZstdError('empty input')
  while pos < len(data):
    if pos + 4 > len(data):
      raise ZstdError('frame magic is truncated')
    (magic,) = struct.unpack_from('<I', data, pos)
    pos += 4
    if magic & _SKIPPABLE_MAGIC_MASK == _SKIPPABLE_MAGIC:
      if pos + 4 > len(data):
        raise ZstdError('skippable frame is truncated')
      (size,) = struct.unpack_from('<I', data, pos)
      pos += 4 + size
      if pos > len(data):
        raise ZstdError('skippable frame is truncated')
      continue
    if magic != _FRAME_MAGIC:
      raise ZstdError(f'bad zstd magic 0x{magic:08X}')
    pos = _decode_frame(data, pos, out, max_output)
  return bytes(out)
