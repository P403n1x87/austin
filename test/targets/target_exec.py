import os
import sys
from pathlib import Path
import time

HERE = Path(__file__).parent

time.sleep(5)
os.execl(sys.executable, "python", str(HERE / "target34.py"))
