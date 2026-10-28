# Logging

Austin uses `syslog` on Linux and macOS, and `%TEMP%\austin.log` on Windows
for log messages, so make sure to watch these to get execution details and
statistics. _Bad_ frames are output together with the other frames. The error
rate depends on the workload: it is generally low, but it can be higher for
applications whose stacks change very rapidly, e.g. with many short calls or
deep recursion.

Logging can be disabled using [environment
variables](usage.md#environment-variables).
