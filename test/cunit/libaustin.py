import sys
import typing as t
from ctypes import CDLL
from ctypes import CFUNCTYPE
from ctypes import POINTER
from ctypes import Structure
from ctypes import c_char_p
from ctypes import c_int
from ctypes import c_ulong
from ctypes import c_void_p
from test.cunit import SRC
from types import ModuleType


la = CDLL(str(SRC / ".libs" / "libaustin.so"))


# ---- libaustin spec ---------------------------------------------------------


class Frame(Structure):
    _fields_ = [
        ("key", c_ulong),
        ("filename", c_char_p),
        ("scope", c_char_p),
        ("line", c_int),
    ]


la.austin_callback = austin_callback = CFUNCTYPE(None, c_int, c_int)

la.austin_up.restype = c_int

la.austin_attach.argtypes = [c_int]
la.austin_attach.restype = c_void_p

la.austin_detach.argtypes = [c_void_p]

la.austin_sample.argtypes = [c_void_p, austin_callback]
la.austin_sample.restype = c_int

la.austin_sample_thread.argtypes = [c_void_p, c_int]
la.austin_sample_thread.restype = c_int

la.austin_pop_frame.restype = POINTER(Frame)

la.austin_read_frame.argtypes = [c_void_p, c_void_p]
la.austin_read_frame.restype = POINTER(Frame)

# -----------------------------------------------------------------------------

sys.modules[__name__] = t.cast(ModuleType, la)
