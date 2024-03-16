import ctypes
import re
from ctypes import CDLL
from ctypes import POINTER
from ctypes import Structure
from ctypes import c_char_p
from ctypes import c_long
from ctypes import c_void_p
from ctypes import cast
from pathlib import Path
from subprocess import PIPE
from subprocess import STDOUT
from subprocess import run
from types import ModuleType
from typing import Any
from typing import Callable
from typing import Optional
from typing import Type

from pycparser import c_ast
from pycparser import c_parser
from pycparser.plyparser import ParseError


HERE = Path(__file__).resolve().parent
TEST = HERE.parent
ROOT = TEST.parent
SRC = ROOT / "src"


restrict_re = re.compile(r"__restrict \w+")

_header_head = r"""
#define __attribute__(x)
#define __extension__
#define __inline inline
#define __asm__(x)
#define __const const
#define __inline__ inline
#define __inline inline
#define __restrict
#define __signed__ signed
#define __GNUC_VA_LIST
#define __gnuc_va_list char
#define __thread
// typedef struct __builtin_va_list { } __builtin_va_list;
"""


def preprocess(source: Path) -> str:
    with source.open() as fin:
        code = _header_head + fin.read()
        return restrict_re.sub(
            "",
            run(
                ["gcc", "-E", "-P", "-"],
                stdout=PIPE,
                input=code.encode(),
                cwd=SRC,
            ).stdout.decode(),
        )


class CompilationError(Exception):
    pass


def compile(
    source: Path,
    cflags: list[str] = [],
    extra_sources: list[Path] = [],
    ldadd: list[str] = [],
    force: bool = False,
) -> None:
    binary = source.with_suffix(".so")

    if not force and binary.is_file():
        return

    result = run(
        [
            "gcc",
            "-shared",
            *cflags,
            "-o",
            str(binary),
            str(source),
            *(str(_) for _ in extra_sources),
            *ldadd,
        ],
        stdout=PIPE,
        stderr=STDOUT,
        cwd=SRC,
    )

    if result.returncode == 0:
        return

    raise CompilationError(result.stdout.decode())


C = CDLL("libc.so.6")


class CFunctionDef:
    def __init__(
        self, name: str, args: list[tuple[str, Type[ctypes._SimpleCData]]], rtype: Any
    ) -> None:
        self.name = name
        self.args = args
        self.rtype = rtype


class CTypeDef:
    def __init__(self, name: str, fields: list[str]) -> None:
        self.name = name
        self.fields = fields
        self.methods = []
        self.constructor = None


class CType(Structure):
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        self.__cself__ = self.new(*args, **kwargs)

    def __del__(self) -> None:
        if len(self.destroy.__cmethod__.__args__) == 1:
            self.destroy()

    def __repr__(self) -> str:
        return f"<{self.__class__.__name__} CObject at {self.__cself__}>"


class CFunction:
    def __init__(self, cfuncdef: CFunctionDef, cfunc: Callable[..., Any]) -> None:
        self.__name__ = cfuncdef.name
        self.__args__ = [_[0] for _ in cfuncdef.args]
        self.__cfunc__ = cfunc

        # Prevent argument values from being truncated/mangled
        self.__cfunc__.argtypes = [_[1] for _ in cfuncdef.args]
        self.__cfunc__.restype = (
            cfuncdef.rtype if cfuncdef.rtype is not None else c_long
        )

        self._posonly = all(_ is None for _ in self.__args__)

    def check_args(self, args: tuple[Any], kwargs: dict[str, Any]) -> None:
        if self._posonly and kwargs:
            raise ValueError(f"{self} takes only positional arguments")

        nargs = len(args) + len(kwargs)
        if nargs != len(self.__args__):
            raise TypeError(
                f"{self} takes exactly {len(self.__args__)} arguments ({nargs} given)"
            )

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        self.check_args(args, kwargs)
        return self.__cfunc__(*args, **kwargs)

    def __repr__(self) -> None:
        return f"<CFunction '{self.__name__}'>"


class CMethod(CFunction):
    def __init__(
        self, cfuncdef: CFunctionDef, cfunc: Callable[..., Any], ctype: Any
    ) -> None:
        super().__init__(cfuncdef, cfunc)
        self.__ctype__ = ctype

    def __get__(self, obj: Any, objtype: Optional[Type] = None) -> None:
        def _(*args, **kwargs):
            cargs = [obj.__cself__, *args]
            self.check_args(cargs, kwargs)

            return self.__cfunc__(*cargs, **kwargs)

        _.__cmethod__ = self

        return _

    def __repr__(self) -> str:
        return f"<CMethod '{self.__name__}' of CType '{self.__ctype__.__name__}'>"


class CStaticMethod(CFunction):
    def __init__(
        self, cfuncdef: CFunctionDef, cfunc: Callable[..., Any], ctype: Any
    ) -> None:
        super().__init__(cfuncdef, cfunc)
        self.__ctype__ = ctype

    def __repr__(self) -> str:
        return f"<CStaticMethod '{self.__name__}' of CType '{self.__ctype__.__name__}'>"


class CMetaType(type(Structure)):
    def __new__(
        cls, cmodule: "CModule", ctypedef: CTypeDef, _: Optional[Any] = None
    ) -> Type["CMetaType"]:
        ctype = super().__new__(
            cls,
            ctypedef.name,
            (CType,),
            {"__cmodule__": cmodule},
        )

        constructor = getattr(cmodule.__binary__, f"{ctypedef.name[:-2]}_new")
        ctype.new = CStaticMethod(ctypedef.constructor, constructor, ctype)

        for method_def in ctypedef.methods:
            method_name = method_def.name
            method = getattr(cmodule.__binary__, f"{ctypedef.name[:-2]}__{method_name}")
            setattr(ctype, method_name, CMethod(method_def, method, ctype))

        ctype.__cname__ = ctypedef.name

        return ctype


class DeclCollector(c_ast.NodeVisitor):
    def __init__(self) -> None:
        self.types = {}
        self.functions = []

    def _get_type(self, node: c_ast.Node) -> None:
        return self.types[" ".join(node.type.type.names)]

    def visit_Typedef(self, node: c_ast.Node) -> None:
        if isinstance(node.type.type, c_ast.Struct) and node.type.declname.endswith(
            "_t"
        ):
            struct = node.type.type
            self.types[node.type.declname[:-2]] = CTypeDef(
                node.type.declname,
                [decl.name for decl in struct.decls],
            )

    def visit_Decl(self, node: c_ast.Node) -> None:
        if "extern" in node.storage:
            return

        if isinstance(node.type, c_ast.FuncDecl):
            func_name = node.name
            ret_type = node.type.type
            rtype = None
            if isinstance(ret_type, c_ast.PtrDecl):
                if "".join(ret_type.type.type.names) == "char":
                    rtype = c_char_p
                else:
                    rtype = c_void_p

            args = (
                [
                    (
                        (
                            _.name,
                            c_void_p if isinstance(_.type, c_ast.PtrDecl) else c_long,
                        )
                        if hasattr(_, "name")
                        else None
                    )
                    for _ in node.type.args.params
                ]
                if node.type.args is not None
                else []
            )
            if func_name.endswith("_new"):
                self.types[f"{func_name[:-4]}"].constructor = CFunctionDef(
                    "new", args, rtype
                )
            elif "__" in func_name:
                type_name, _, method_name = func_name.partition("__")
                if not type_name:
                    return
                self.types[type_name].methods.append(
                    CFunctionDef(method_name, args, rtype)
                )
            else:
                self.functions.append(CFunctionDef(func_name, args, rtype))

    def collect(self, decl: str) -> dict[str, CTypeDef]:
        parser = c_parser.CParser()
        try:
            ast = parser.parse(decl, filename="<preprocessed>")
        except ParseError as e:
            lines = decl.splitlines()
            line, col = (
                int(_) - 1 for _ in e.args[0].partition(" ")[0].split(":")[1:3]
            )
            for i in range(max(0, line - 4), min(line + 5, len(lines))):
                if i != line:
                    print(f"{i+1:5d}  {lines[i]}")
                else:
                    print(f"{i+1:5d}  \033[33;1m{lines[line]}\033[0m")
                    print(" " * (col + 5) + "\033[31;1m<<^\033[0m")
            raise

        self.visit(ast)
        return {
            k: v
            for k, v in self.types.items()
            if isinstance(v, CTypeDef) and v.constructor is not None
        }


class CModule(ModuleType):
    def __init__(self, source: Path) -> None:
        super().__init__(source.name, f"Generated from {source.with_suffix('.c')}")

        so_file = source.with_suffix(".so")
        while True:
            try:
                self.__binary__ = CDLL(so_file)
                break
            except OSError:
                # On parallel runs we might still be compiling the source file
                # so we try until we succeed.
                if not so_file.exists():
                    raise

        collector = DeclCollector()

        for name, ctypedef in collector.collect(
            preprocess(source.with_suffix(".h"))
        ).items():
            parts = name.split("_")
            py_name = "".join((_.capitalize() for _ in parts))
            setattr(self, py_name, CMetaType(self, ctypedef, None))

        for cfuncdef in collector.functions:
            name = cfuncdef.name
            try:
                cfunc = CFunction(cfuncdef, getattr(self.__binary__, name))
                setattr(self, name, cfunc)
            except AttributeError:
                # Not part of the binary
                pass

    def cglobal(self, name: str, ctype: str) -> Optional[Any]:
        cglobal = getattr(self.__binary__, name)
        if cglobal is None:
            return None

        c_type = getattr(ctypes, f"c_{ctype}")
        if c_type is None:
            return None

        return cast(cglobal, POINTER(c_type)).contents.value

    @classmethod
    def compile(
        cls,
        source: Path,
        cflags: list[str] = [],
        extra_sources: list[Path] = [],
        ldadd: list[str] = [],
    ):
        compile(source.with_suffix(".c"), cflags, extra_sources, ldadd)
        return cls(source)
