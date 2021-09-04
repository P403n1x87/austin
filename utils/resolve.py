import os
import sys
import typing as t
from subprocess import check_output


def demangle_cython(function: str) -> str:
    for i, d in enumerate(function):
        if d.isdigit():
            break
    else:
        raise ValueError("Invalid Cython mangled name")

    n = 0
    while i < len(function):
        c = function[i]
        i += 1
        if c.isdigit():
            n = n * 10 + int(c)
        else:
            i += n
            n = 0
            if not function[i].isdigit():
                return function[i:]


class Maps:
    def __init__(self):
        # TODO: Use an interval tree instead!
        self.maps: t.List[t.Tuple(int, int, str)] = []
        self.bases = {}
        self.cache = {}

    def addr2line(self, address: str) -> t.Optional[t.Tuple[str, t.Optional[str]]]:
        if address in self.cache:
            return self.cache[address]

        addr = int(address, 16)
        for lo, hi, binary in self.maps:
            if lo <= addr <= hi:
                break
        else:
            self.cache[address] = None
            return None

        resolved, _, _ = (
            check_output(["addr2line", "-e", binary, f"{addr-self.bases[binary]:x}"])
            .decode()
            .strip()
            .partition(" ")
        )
        if resolved.startswith("??"):
            # self.cache[address] = (f"{binary}@{addr-self.bases[binary]:x}", None)
            self.cache[address] = (f"{binary}", addr - self.bases[binary])
            return self.cache[address]

        self.cache[address] = tuple(resolved.split(":", maxsplit=1))
        return self.cache[address]

    def add(self, line: str) -> None:
        bounds, _, binary = line[7:].strip().partition(" ")
        low, _, high = bounds.partition("-")
        lo = int(low, 16)
        hi = int(high, 16)
        self.maps.append((lo, hi, binary))
        if binary in self.bases:
            self.bases[binary] = min(self.bases[binary], lo)
        else:
            self.bases[binary] = lo

    def resolve(self, line: str) -> str:
        parts = []
        frames, _, metrics = line.strip().rpartition(" ")
        for part in frames.split(";"):
            if part.startswith("native@"):
                head, function, lineno = part.split(":")
                if function.startswith("__pyx_pw_"):
                    # skip Cython wrappers (cpdef)
                    continue
                _, _, address = head.partition("@")
                resolved = self.addr2line(address)
                if resolved is None:
                    parts.append(part)
                else:
                    source, native_lineno = resolved
                    if function.startswith("__pyx_"):
                        function = demangle_cython(function)
                    parts.append(f"{source}:{function}:{native_lineno or lineno}")
            else:
                parts.append(part)

        return " ".join((";".join(parts), metrics))


def main():
    try:
        stats = sys.argv[1]
        assert os.isfile(stats)
    except IndexError:
        print("Usage: python resolve.py <austin-file>", file=sys.stderr)
        sys.exit(1)
    except AssertionError:
        print("Austin file does not exist", file=sys.stderr)
        sys.exit(1)

    maps = Maps()
    with open(stats) as s:
        for line in s:
            if line.startswith("# map: "):
                maps.add(line)
            elif line.startswith("# ") or line == "\n":
                print(line, end="")
            else:
                print(maps.resolve(line))


if __name__ == "__main__":
    main()
