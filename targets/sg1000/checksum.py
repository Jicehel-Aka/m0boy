"""LPC boot ROM: the first eight vector words must sum to zero."""
import struct
import sys
from pathlib import Path
path = Path(sys.argv[1])
image = bytearray(path.read_bytes())
struct.pack_into('<I', image, 28, (-sum(struct.unpack_from('<7I', image))) & 0xffffffff)
path.write_bytes(image)
