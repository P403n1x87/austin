import os
import socket
import sys
from pathlib import Path

HERE = Path(__file__).parent

port = int(sys.argv[1])

# Block until the test process connects, instead of guessing how long our
# own startup plus the profiler's initial attach might take on a given
# runner. The connection itself is the "go ahead and exec" signal -- accept()
# can't return before it arrives, so there's nothing to race.
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("localhost", port))
    srv.listen(1)
    conn, _ = srv.accept()
    conn.close()

os.execl(sys.executable, "python", str(HERE / "target34.py"))
