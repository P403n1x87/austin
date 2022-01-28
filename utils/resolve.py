import os
import sys
import typing as t
from subprocess import check_output


def demangle_cython(function: str) -> str:
    if function.startswith("__pyx_pymod_"):
        _, _, function = function[12:].partition("_")
        return function

    if function.startswith("__pyx_fuse_"):
        function = function[function[12:].index("__pyx_") + 12 :]
    for i, d in enumerate(function):
        if d.isdigit():
            break
    else:
        raise ValueError(f"Invalid Cython mangled name: {function}")

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

    return function


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
            check_output(["addr2line", "-Ce", binary, f"{addr-self.bases[binary]:x}"])
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
            try:
                head, function, lineno = part.split(":")
            except ValueError:
                parts.append(part)
                continue
            if function.startswith("__pyx_pw_") or function.startswith("__pyx_pf_"):
                # skip Cython wrappers (cpdef)
                continue
            if function.startswith("__pyx_"):
                function = demangle_cython(function)
            if head.startswith("native@"):
                _, _, address = head.partition("@")
                resolved = self.addr2line(address)
                if resolved is None:
                    parts.append(":".join((head, function, lineno)))
                else:
                    source, native_lineno = resolved
                    parts.append(f"{source}:{function}:{native_lineno or lineno}")
            else:
                parts.append(":".join((head, function, lineno)))

        return " ".join((";".join(parts), metrics))


def main():
    try:
        stats = sys.argv[1]
        assert os.path.isfile(stats)
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
