"""Target for testing native mode sampling of non-Python OS threads.

Imports native_ext (a C extension built from test/native/) and starts two
native threads named "Native-0" and "Native-1" that have no PyThreadState.  Austin
should observe both threads alongside the Python main thread.
"""

import native_ext
import time

token = native_ext.start_threads(2)

time.sleep(1)

native_ext.join_threads(token)
