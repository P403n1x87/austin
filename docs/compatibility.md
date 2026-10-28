# Compatibility

Austin supports Python 3.10 through 3.15, including **free-threaded builds**,
and has been tested on the following platforms and architectures

|             | Linux | Windows | macOS |
| ----------- | :---: | :-----: | :---: |
| **x86_64**  | ✓     | ✓       | ✓     |
| **i686**    | ✓     |         |       |
| **armv7**   | ✓     |         |       |
| **arm64**   | ✓     |         | ✓     |
| **ppc64le** | ✓     |         |       |

!!! note
    Memory profiling (`-m`/`--memory`) in free-threaded Python builds is less
    accurate, as without the GIL there is no reliable way of attributing memory
    deltas to the thread that caused them.

If you are looking for support for older versions of CPython, the following
table summarises the compatibility of Austin with CPython versions

| CPython Versions                     | Austin Version |
| ------------------------------------ | -------------- |
| 2.3-2.7, 3.3-3.11                    | 3.5            |
| 3.8-3.13                             | 3.7            |
| 3.9-3.14                             | 4.0            |
| 3.10-3.15, (incl. free-threaded)     | 4.1            |

!!! note
    Austin *might* work with other platforms and architectures not listed
    above. So it is worth giving it a try even if your system is not listed. If
    you are looking for support for Python < 3.8, you can use Austin 3.5.

Because of platform-specific details, Austin usage may vary slightly. Below are
further compatibility details to be aware of.


## On Linux

Austin requires the `CAP_SYS_PTRACE` capability to attach to an external
process. This means that you will have to either use `sudo` when attaching to
a running Python process or grant the `CAP_SYS_PTRACE` capability to the Austin
binary with, e.g.

~~~ console
sudo setcap cap_sys_ptrace+ep `which austin`
~~~

To use Austin with Docker, the `--cap-add SYS_PTRACE` option needs to be passed
when starting a container.


## On macOS

Due to the **System Integrity Protection**, introduced in **macOS** with El
Capitan, and the [Hardened Runtime][hardened runtime], introduced in Mojave,
Austin cannot profile Python processes that use an executable located in the
`/bin` folder, or code-signed, even with `sudo`. This is the case for the
system-provided version of Python, and the one installed with the official
installers from [python.org](https://python.org). Other installation methods,
like [pyenv] or [Anaconda][anaconda] or
[Homebrew](https://formulae.brew.sh/formula/austin) are known to work with
Austin, out of the box.

To use Austin with Python from the official installer, you could remove the
signature from the binaries with

~~~ console
codesign --remove-signature /Library/Frameworks/Python.framework/Versions/<M.m>/bin/python3
codesign --remove-signature /Library/Frameworks/Python.framework/Versions/<M.m>/Resources/Python.app/Contents/MacOS/Python
~~~

Alternatively, you could self-sign the Austin binary with the [Debugging Tool
Entitlement][dte], as done for debugging tools like GDB. However, this method
has not been tested.

!!! tip
    Austin requires the use of `sudo` to work on macOS. To avoid having to type
    the password every time you use Austin, consider adding a rule to the
    `sudoers` file, e.g.

    ~~~
    yourusername  ALL = (root) NOPASSWD: /usr/local/bin/austin
    ~~~


[anaconda]: https://www.anaconda.com/
[dte]: https://developer.apple.com/documentation/bundleresources/entitlements/com_apple_security_cs_debugger
[hardened runtime]: https://developer.apple.com/documentation/security/hardened_runtime
[pyenv]: https://github.com/pyenv/pyenv
