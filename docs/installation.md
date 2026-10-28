# Installation

Austin is available to install from [PyPI] and from the major software
repositories of the most popular platforms. Check out the [latest release] page
for pre-compiled binaries and installation packages.

On all supported platforms and architectures, Austin can be installed from PyPI
with `pip` or `pipx` via the commands

~~~ console
pip install austin-dist
~~~

or

~~~ console
pipx install austin-dist
~~~

On Linux, it can be installed using `autotools` or as a snap from the [Snap
Store](https://snapcraft.io/store). The latter will automatically perform the
steps of the `autotools` method with a single command. On distributions derived
from Debian, Austin can be installed from the official repositories with
`apt`. Anaconda users can install Austin from [Conda Forge].

On Windows, Austin can be easily installed from the command line using either
[Chocolatey] or [Scoop]. Alternatively, you can download the installer from the
[latest release] page.

On macOS, Austin can be easily installed from the command line using [Homebrew].
Anaconda users can install Austin from [Conda Forge].

For any other platforms, compiling Austin from sources is as easy as cloning the
repository and running the C compiler. The [Releases][releases] page has many
pre-compiled binaries that are ready to be uncompressed and used.


## With `autotools`

Installing Austin using `autotools` amounts to the usual `./configure`, `make`
and `make install` finger gymnastic. The only dependency is the standard C
library. Before proceeding with the steps below, make sure that the `autotools`
are installed on your system. Refer to your distro's documentation for details
on how to do so.

~~~ console
git clone --depth=1 https://github.com/P403n1x87/austin.git && cd austin
autoreconf --install
./configure
make
make install
~~~

!!! note
    Some Linux distributions, like Manjaro, might require the execution of
    `automake --add-missing` before `./configure`.

Alternatively, sources can be compiled with just a C compiler (see
[below](#from-sources-without-autotools)).


## From the Snap Store

Austin can be installed on [many major Linux
distributions](https://snapcraft.io/docs/installing-snapd) from the Snap Store
with the following command

~~~ console
sudo snap install austin --classic
~~~


## On Debian and Derivatives

On March 30 2019 Austin was accepted into the official Debian repositories and
can therefore be installed with the `apt` utility.

~~~ console
sudo apt update -y && sudo apt install austin -y
~~~


## On macOS

Austin can be installed on macOS using [Homebrew](https://docs.brew.sh):

~~~ console
brew install austin
~~~


## From Chocolatey

To install [Austin from Chocolatey](https://chocolatey.org/packages/austin), run
the following command from the command line or from PowerShell

~~~ console
choco install austin
~~~

To upgrade run the following command from the command line or from PowerShell:

~~~ console
choco upgrade austin
~~~


## From Scoop

To install Austin using Scoop, run the following command from the command line
or PowerShell

~~~ console
scoop install austin
~~~

To upgrade run the following command from the command line or PowerShell:

~~~ console
scoop update
~~~


## From Conda Forge

Anaconda users on Linux and macOS can install Austin from [Conda Forge] with the
command

~~~ console
conda install -c conda-forge austin
~~~


## From Sources without `autotools`

To install Austin from sources using the GNU C compiler, without `autotools`,
clone the repository with

~~~ console
git clone --depth=1 https://github.com/P403n1x87/austin.git
~~~

On Linux, one can then use the command

~~~ console
gcc -O3 -Os -Wall -pthread src/*.c -o src/austin
~~~

whereas on macOS it is enough to run

~~~ console
gcc -O3 -Os -Wall src/*.c -o src/austin
~~~

On Windows, the `-lpsapi -lntdll -ldbghelp` switches are needed

~~~ console
gcc -O3 -Os -Wall -lpsapi -lntdll -ldbghelp src/*.c -o src/austin
~~~

Add `-DDEBUG` if you need a more verbose log. This is useful if you encounter a
bug with Austin and you want to report it.


[Chocolatey]: https://chocolatey.org/
[Conda Forge]: https://anaconda.org/conda-forge/austin
[Homebrew]: https://formulae.brew.sh/formula/austin
[latest release]: https://github.com/P403n1x87/austin/releases/latest
[PyPI]: https://pypi.org/project/austin-dist/
[releases]: https://github.com/P403n1x87/austin/releases
[Scoop]: https://scoop.sh/
