from enum import Enum


class AustinError(int, Enum):
    __message__ = {
        0: "No error",
        1: "Operating system error",
        2: "Permission error",
        3: "Memory copy error",
        4: "Memory allocation error",
        5: "I/O error",
        6: "Command line error",
        7: "Environment error",
        8: "Value error",
        9: "Null pointer error",
        10: "Python version error",
        11: "Binary analysis error",
        12: "Python object error",
        13: "VM maps error",
        14: "Iteration ended error",
    }

    __fatal__ = {
        0: False,
        1: True,
        2: True,
        3: True,
        4: True,
        5: True,
        6: True,
        7: True,
        8: False,
        9: True,
        10: True,
        11: False,
        12: False,
        13: False,
        14: False,
    }

    OK = 0
    OS = 1
    PERM = 2
    MEMCOPY = 3
    MALLOC = 4
    IO = 5
    CMDLINE = 6
    ENV = 7
    VALUE = 8
    NULL = 9
    VERSION = 10
    BINARY = 11
    PYOBJECT = 12
    VM = 13
    ITEREND = 14

    @classmethod
    def message(cls, code: "AustinError") -> str:
        """Return the error message for the given error code."""
        return cls.__message__[code]

    @classmethod
    def is_fatal(cls, code: "AustinError") -> bool:
        """Return True if the error code is fatal."""
        return cls.__fatal__[code]
