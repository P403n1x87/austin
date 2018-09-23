![austin](art/austin.png)

Meet Austin, a Python frame stack sampler for CPython.

# Synopsis

Austin is a Python frame stack sampler for CPython written in pure C. It samples
the stack traces of a Python application and provides samples that can be later
on analysed for further statistics.

The most interesting use of Austin is probably in conjunction with FlameGraph to
profile the Python code as it is being interpreted by CPython.

However, the output format can be grabbed from any other external tool for
further processing. Look, for instance, at the following Python TUI, similar in
spirit to [py-spy](https://github.com/benfred/py-spy).

![tui](art/austin-tui_wip.png)

The current version only supports the 64-bit version of Python 3.6 on
Linux-based operating systems that has not been compiled wit the
`--enable-shared` flag. There is a plan to first support more Python versions,
then the 32-bit architecture and finally more operating systems.

Presently, it is only possible to start a Python process with Austin. There are
plans to allow you to attach Austin to a running Python application.


# Installation

Installing Austin amounts to the usual `./configure`, `make` and `make install`
finger gymnastic. The only dependency is the standard C library.

~~~ bash
git clone --depth=1 https://github.com/P403n1x87/austin.git
autoreconf --install
./configure
make
make install
~~~

Compilation has been tested with GNU GCC 7.3.0. The code is so simple that it
really compiles with just

~~~ bash
gcc -O3 -Wall `ls src/*.c` -o austin
~~~

Add `-DDEBUG` if you want a more verbose syslog.

# Usage

~~~
Usage: austin [OPTION...] executable [args ...]
austin -- A frame stack sampler for Python.

  -i, --interval=n_usec      Sampling interval (default is 500 usec)
  -?, --help                 Give this help list
      --usage                Give a short usage message
~~~

The output is a sequence of frame stack samples, one on each line. The format is
the collapsed one that is recognised by
[FlameGraph](https://github.com/brendangregg/FlameGraph) so that it can be piped
to `flamegraph.pl` in order to produce flame graphs, or redirected to a file for
some further processing.

Each line has the structure

~~~
Thread [tid];[func] ([mod]);#[line no];[func] ...;#[line no] [usec]
~~~

The reason why the line number is not included in the `([mod])` part, as done
by py-spy, is that, this way, the flame graph will show the total time spent at
each function, plus the finer detail of the time spent on each line.

Austin uses syslog for log messages so make sure you watch `/var/log/syslog` for
the `austin` tag to get some execution details and statistics.

# How does it work?

To understand how Python works internally in terms of keeping track of all the
function calls, you can have a look at the related project
[py-spy](https://github.com/benfred/py-spy) and references therein (in
particular the [pyflame](https://github.com/uber/pyflame) project).

The approach taken by Austin is similar to `py-spy` in the sense that it too
peeks at the process memory while it is running, instead of pausing it with
`ptrace`.

Austin will fork itself and execute the program given at the command line. It
waits until python has mapped its memory and uses the information contained in
`/proc/[pid]/maps` to retrieve the location of the binary. It then looks at its
header in memory to find the location and size of the section header table and
maps the executable in memory.

The next thing that we need is the Python version, which is contained in the
`.rodata` section. It is located somewhere at the beginning of it, but since we
don't know exactly where, we scan for a string that looks like a Python version.
Based on the result we determine how to proceed, as the next step are, in
general, dependant on the Python release (due to changes in the ABI that can
happen even across different minor versions). For the time being, only version
3.6 is supported.

In order to get to the interpreter state, we can choose between two approaches.
One is to scan the `.bss` section multiple times until we find a pointer to the
heap that points to an instance of `PyInterpreterState`. To determine whether it
is a valid pointer or not we can use the relation between `PyInterpreterState`
and the `PyThreadState` it points to with the `tstate_head` field. Indeed, the
`PyThreadState` instance has a field, `interp`, that points back to the
`PyInterpreterState` instance. So, if we find this cyclic reference, we can be
quite certain that we have found the `PyInterpreterState` instance that we were
looking for.

Another approach is to look for the `_PyThreadState_Current` symbol in the
`.dynsym` section, which is just a pointer to an instance of `PyThreadState`,
and watch its de-referenced value until it points to a valid
`PyInterpreterState` instance. For the check we can use the same criterion as
before, bearing in mind that, in principle, the `tstate_head` of
`PyInterpreterState` can point to some other instance of `PyThreadState`.

At this point we are ready to navigate all the threads and traverse their frame
stacks at regular interval of times to sample them.

## Concurrency

Since the python process being sampled is not stopped, but a snapshot of its
threads and frame stacks is taken on the fly, some samples might end up being
invalid. Given that the Austin is written in pure C, it is bound to outperform
the Python process and give statistically reliable result at high sampling
rates.

Plans for the future involve the development of a hybrid mode that would allow
Austin to determine whether it is the case to pause the Python process in case
of a high invalid rate. Such a mode should be implemented judiciously, as even
when pausing the Python process there is no guarantee that one can read a valid
interpreter state. For example, it can easily happen that Austin decides to
pause the Python process while CPython is in the middle of updating the frame
stack. In this case, we would still read some potentially invalid or stale
memory references. A solution is to step over a few instructions and try again,
but even this approach doesn't guarantee 100% accuracy. It might be that a new
read now succeeds, but there is no way of telling whether the references are
genuine or not.

## In between Python releases

The early development of this project was tested against Python 3.6.5. The
method for finding a valid instance of `PyInterpreterState` described above
worked perfectly. After the upgrade to Python 3.6.6, the code stopped working.
It turned out that the following "fix" was required

~~~ C
// 3.6.5 -> 3.6.6: _PyThreadState_Current doesn't seem what one would expect
//                 anymore, but _PyThreadState_Current.prev is.
if (tstate_current.thread_id == 0 && tstate_current.prev != 0)
  self->tstate_curr_raddr = tstate_current.prev;
~~~

That is, in Python 3.6.6, the symbol `_PyThreadState_Current` does not
de-reference to a "valid" instance of `PyThreadState`, but `PythreadState.prev`
does. Fortunately enough, this fix works for both versions of Python.


# Examples

The following flame graph has been obtained with the command

~~~
./austin -i 500 ./test.py | ./flamegraph.pl --colors mem --countname=usec > test.svg
~~~

where the sample `test.py` script has the following content

~~~ python
#!/usr/bin/env python3

import threading

def keep_cpu_busy():
    a = []
    for i in range(10_000_000):
        a.append(i)

if __name__ == "__main__":
    threading.Thread(target=keep_cpu_busy).start()
    keep_cpu_busy()
~~~

![test_graph](art/test.png)

The tall stack on the right is the initialisation phase of the Python
interpreter.


----


# License

GNU GPLv3
