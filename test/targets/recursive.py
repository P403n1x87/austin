def sum_up_to(n):
    if n <= 1:
        return 1

    result = n + sum_up_to(n - 1)

    return result


for _ in range(200000):
    N = 16
    assert sum_up_to(N) == (N * (N + 1)) >> 1
