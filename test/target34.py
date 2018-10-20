#!/usr/bin/env python3

import threading

def keep_cpu_busy():
    a = []
    for i in range(10000000):
        a.append(i)

if __name__ == "__main__":
    threading.Thread(target=keep_cpu_busy).start()
    keep_cpu_busy()
    print("Done")
