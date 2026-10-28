# Ecosystem

Austin's [MOJO](formats/mojo.md) output can be consumed by a number of tools,
or by your own, with the help of the [`austin-python`] package.


## IDE Extensions

It is easy to write your own extension for your favourite text editor. This, for
example, is a demo of the [Visual Studio Code] extension that highlights the
most hit lines of code straight into the editor

![Austin VS Code extension demo](assets/art/vscode-demo.gif)


## Austin TUI

The [Austin TUI] is a text-based user interface for Austin that gives you a
top-like view of what is currently running inside a Python application. It is
most useful for scripts that have long-running procedures as you can see where
execution is at without tracing instructions in your code. You can also save the
collected data from within the TUI and feed it to Flame Graph for visualisation,
or convert it to the [pprof] format.

If you want to give it a go you can install it using `pip` with

~~~ console
pip install austin-tui --upgrade
~~~

and run it with

~~~ console
austin-tui [OPTION...] command [ARG...]
~~~

with the same command line as Austin. Please note that the `austin` binary
should be available from within the `PATH` environment variable in order for the
TUI to work.

!!! warning "Important"
    The TUI is based on `python-curses`. The version included with the standard
    Windows installations of Python is broken so it won't work out of the box. A
    solution is to install the wheel of the port to Windows from
    [this](https://www.lfd.uci.edu/~gohlke/pythonlibs/#curses) page. Wheel files
    can be installed directly with `pip`, as described in the
    [linked](https://pip.pypa.io/en/latest/user_guide/#installing-from-wheels)
    page.

![Austin TUI](assets/art/austin-tui.gif)


## Austin Web

[Austin Web] is a web application that wraps around Austin. At its core, Austin
Web is based on [d3-flame-graph] to display a _live_ flame graph in the browser,
that refreshes every 3 seconds with newly collected samples. Austin Web can also
be used for _remote_ profiling by setting the `--host` and `--port` options.

If you want to give it a go you can install it using `pip` with

~~~ console
pip install austin-web --upgrade
~~~

and run it with

~~~ console
austin-web [OPTION...] command [ARG...]
~~~

with the same command line as Austin. This starts a simple HTTP server that
serves on `localhost` by default. When no explicit port is given, Austin Web
will use an ephemeral one.

Please note that the `austin` binary should be available from within the `PATH`
environment variable in order for Austin Web to work.

![Austin Web](assets/art/austin-web.gif)


## Speedscope

Austin's MOJO output can be converted to the [Speedscope] JSON format with the
`austin2speedscope` utility that comes with the [`austin-python`] package.

If you want to give it a go you can install it using `pip` with

~~~ console
pip install austin-python --upgrade
~~~

and run it with

~~~ console
austin2speedscope [-h] [--indent INDENT] [-V] input output
~~~

where `input` is a file containing the output from Austin and `output` is the
name of the JSON file to use to save the result of the conversion, ready to be
used on [Speedscope].

![Speedscope](assets/art/speedscope.png)


## Google pprof

Austin's format can also be converted to the Google pprof format using the
`austin2pprof` utility that comes with [`austin-python`]. If you want to give it
a go you can install it using `pip` with

~~~ console
pip install austin-python --upgrade
~~~

and run it with

~~~ console
austin2pprof [-h] [-V] input output
~~~

where `input` is a file containing the output from Austin and `output` is the
name of the protobuf file to use to save the result of the conversion, ready to
be used with [Google's pprof tools][pprof].


[Austin TUI]: https://github.com/P403n1x87/austin-tui
[Austin Web]: https://github.com/P403n1x87/austin-web
[`austin-python`]: https://github.com/P403n1x87/austin-python
[d3-flame-graph]: https://github.com/spiermar/d3-flame-graph
[pprof]: https://github.com/google/pprof
[Speedscope]: https://speedscope.app
[Visual Studio Code]: https://marketplace.visualstudio.com/items?itemName=p403n1x87.austin-vscode
