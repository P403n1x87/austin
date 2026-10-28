# Austin

![Austin](assets/art/austin_die_cut_sticker.png){ width="160" align="right" }

Austin is a Python frame stack sampler for CPython written in pure C. Samples
are collected by reading the CPython interpreter virtual memory space to
retrieve information about the currently running threads along with the stack of
the frames that are being executed. Hence, one can use Austin to easily make
powerful statistical profilers that have minimal impact on the target
application and that don't require any instrumentation.

The key features of Austin are:

- Zero instrumentation;
- Minimal impact;
- Fast and lightweight;
- Time and memory profiling;
- Built-in support for multi-process applications (e.g. `mod_wsgi`).

The simplest way to turn Austin into a full-fledged profiler is to use it
together with the [VS Code extension][Austin VS Code extension] or combine it
with [FlameGraph] or [Speedscope]. However, Austin's binary output can be piped
into any other external or custom tools for further processing. Look, for
instance, at the following Python TUI

![Austin TUI](assets/art/austin-tui.png)

Check out [A Survey of Open-Source Python
Profilers](https://www.usenix.org/system/files/login/articles/login_winter19_12_norton.pdf)
by Peter Norton for a general overview of Austin. The
[Ecosystem](ecosystem.md) page has more tool ideas and examples.

!!! note
    This documentation is published at
    <https://p403n1x87.github.io/austin/>, with one version for each Austin
    minor release. If you are browsing these files on GitHub, some elements,
    such as admonitions like this one, won't render as intended.


## Why Austin

When there already are similar tools out there, it's normal to wonder why one
should be interested in yet another one. So here is a list of features that
currently distinguish Austin.

- **Written in pure C** Austin is written in pure C code. There are no
  dependencies on third-party libraries except for the standard C library and
  the API provided by the Operating System.

- **Just a sampler** Austin is just a frame stack sampler. It looks into a
  running Python application at regular intervals of time and dumps whatever
  frame stack it finds. The samples can then be analysed at a later time so that
  Austin can sample at rates higher than other non-C alternatives that perform
  some aggregations at run-time.

- **Compact output, powerful tools** Austin uses the compact
  [MOJO](formats/mojo.md) binary format, which can be easily parsed with the
  [`austin-python`] package, or converted to other popular formats. You can then
  go and build your own tool to analyse Austin's output. You could even make a
  _player_ that replays the application execution in slow motion, so that you
  can see what has happened in temporal order.

- **Small size** Austin compiles to a single binary executable of just a bunch
  of KB.

- **Easy to maintain** Occasionally, the Python C API changes and Austin will
  need to be adjusted to new releases. However, given that Austin, like CPython,
  is written in C, implementing the new changes is rather straight-forward.


## Sampling Accuracy

Austin can sample stacks very rapidly, guaranteeing high accuracy, as well as
allowing to reach very high sampling rates, if required. This makes Austin
suitable for profiling applications in production with almost no compromise
between accuracy and performance.

The rate of samples that cannot be collected depends on the workload: it is
generally low, but it can be higher for applications whose stacks change very
rapidly, e.g. with many short calls or deep recursion. See the
[Logging](logging.md) page for how to find the sampling statistics of a run.


## Support Austin

Austin is a free and open-source project. A lot of effort goes into its
development to ensure the best performance and that it stays up-to-date with
the latest Python releases. If you find it useful, consider
[sponsoring](https://github.com/sponsors/P403n1x87) this project, or have a look
at the [contributing
guidelines](https://github.com/P403n1x87/austin/blob/main/CONTRIBUTING.md) if
you want to help with the development.


[Austin VS Code extension]: https://marketplace.visualstudio.com/items?itemName=p403n1x87.austin-vscode
[`austin-python`]: https://github.com/P403n1x87/austin-python
[FlameGraph]: https://github.com/brendangregg/FlameGraph
[Speedscope]: https://speedscope.app
