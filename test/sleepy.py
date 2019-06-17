import time


def cpu_bound():
    a = []
    for i in range(100000):
        a.append(i)


if __name__ == "__main__":
    for n in range(2):
        cpu_bound()
        time.sleep(1)
