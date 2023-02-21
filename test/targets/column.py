from time import sleep


def lazy(n):
    for _ in range(n):
        sleep(0.1)
        yield _


def fib(n):
    a, b = 0, 1
    for _ in range(n):
        yield a
        a, b = b, a + b

a = [
    list(fib(_))
    for _ in lazy(30)
]

print(a)
