import platform
from setuptools import Extension, setup

extra_compile_args = []
extra_link_args = []

if platform.system() != "Windows":
    extra_compile_args = ["-pthread"]
    extra_link_args = ["-pthread"]

setup(
    ext_modules=[
        Extension(
            "native_ext",
            sources=["native_ext.c"],
            extra_compile_args=extra_compile_args,
            extra_link_args=extra_link_args,
        )
    ]
)
