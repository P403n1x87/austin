# The MOJO File Format

This page describes Austin's MOJO binary file format for version 4. At the high
level, it consists of a stream of events that correspond to the various parts
of Austin's default output format. Each MOJO file has a header with a magic
sequence of bytes and a version number.


## Data types

There are only two fundamental data types used by the MOJO format: strings and
varints. A string is a null-terminated sequence of bytes. A varint is an integer
of variable size which is encoded as follows: the most significant bit of each
byte determines whether the next byte is part of the number. The second most
significant bit of the first byte is the sign bit. Hence, only the last 6 least
significant bits of the first byte contribute to the value of the integer,
whereas for each of the subsequent bytes, the number of bits is 7. For example,
the byte sequence `C3 02` encodes the integer `-131`.


## Header

A MOJO file starts with the byte sequence

| Byte 0 .. 2 | Byte 3 ..      |
| ----------- | -------------- |
| `MOJ`       | version varint |

that is, the byte sequence `MOJ` followed by the varint encoding of the format
version. The initial version is 1. The latest version is 4.


## Events

Each event has the following structure

| Byte 0   | Byte 1 .. n |
| -------- | ----------- |
| Event ID | Event data  |

Note that some events might not have any additional event data. The currently
supported event IDs are listed in the following table.

| Event ID | Name | Event Data | Description |
| -------- | ---- | ---------- | ----------- |
| 0 | | | Reserved. |
| 1 | Metadata | *key*: `string`, *value*: `string` | Metadata key-value pair, e.g. Austin version, detected Python version, sampling metrics etc... . |
| 2 | Stack | *pid*: `varint`, *iid*: `varint`, *thread*: `string` | This signals the beginning of a sample. The event data includes the PID, the interpreter ID, and the thread identifier. The thread identifier is the name of the thread, as known to the `threading` module (e.g. `MainThread`), when it can be resolved, or the thread ID otherwise. Every new stack event signals the end of the previous sample (if any) and the beginning of a new one. |
| 3 | Frame | *key*: `varint`, *filename_key*: `varint`, *scope_key*: `varint`, *line*: `varint`, *line_end*: `varint`, *column*: `varint`, *column_end*: `varint` | This event carries information about a frame. The event data consists of the frame key identifier, two string references for the file path and the function name respectively, and the location information. The location information consists of 4 numbers: the start and end line, and the start and end column. A value of 0 indicates that no information is available for that location value. See [below](#native-frames) for how native frames are encoded. |
| 4 | Invalid frame | | Emitted when an invalid frame is detected. Not emitted since Austin 4.0, but kept for compatibility with older versions. |
| 5 | Frame reference | *frame_key*: `varint` | A reference to a frame by key identifier. These events define the actual frame content of frame stacks. |
| 6 | Kernel frame | *symbol*: `string` | Emitted by the austinp variant to report a kernel frame. The event data is a single string with the name of the kernel symbol. |
| 7 | Garbage collector | | Emitted if the garbage collector is running while sampling with the garbage collector option. |
| 8 | Idle stack | | Emitted if the stack is idle (only in full mode). |
| 9 | Time metric | *value*: `varint` | A time delta in microseconds |
| 10 | Memory metric | *value*: `varint` | A memory delta in bytes. |
| 11 | String event | *key*: `varint`, *value*: `string` | This is a pair of key followed by a literal string. Used to provide a mapping between a string and a string reference, which is used to reduce redundancy. |
| 12 | String reference | *string_key*: `varint` | A reference to a string by key. |
| 13 | Stack repeat | | The Python frames of the current sample are the same as those of the previous sample of the same thread (same PID and thread identifier). See [below](#stack-repeats) for details. |
| 14 | Task stack | *task_id*: `varint`, *name_key*: `varint`, *frame_count*: `varint` | Reports the current coroutine stack of an asyncio task. The event data consists of the task identifier, a string reference to the task name (0 if not available), and the number of frame references that follow and make up the task stack. See [below](#asyncio-tasks) for details. |
| 15 | Task waiter | *task_id*: `varint`, *waiter_id*: `varint` | An edge of the asyncio task graph: the task with identifier *task_id* is being awaited by the task with identifier *waiter_id*. A *waiter_id* of 0 means that the task has no waiters. See [below](#asyncio-tasks) for details. |
| 16 | Running task | *task_id*: `varint` | The asyncio task that the thread of the current sample is running. See [below](#asyncio-tasks) for details. |


## Samples

A sample is made of the following sequence of events, in this order

1. a Stack event;
2. optionally, any asyncio task events (Task stack, Task waiter, Running task)
   for the tasks owned by the thread of the sample;
3. optionally, a Stack repeat event;
4. the Frame reference events of the sample, from the outermost to the
   innermost frame, followed by any Kernel frame events;
5. optionally, a Garbage collector event;
6. the metric events: a Time metric or a Memory metric event, or, in full mode,
   a Time metric event, an optional Idle stack event and a Memory metric event.

Frame and String events, that define new frames and strings, can appear
anywhere in the stream before they are first referenced. Metadata events are
mostly emitted at the beginning and at the end of the stream, outside of any
sample.


## Stack repeats

To reduce the cost of sampling, and the size of MOJO files, Austin can detect
when the Python frames of a thread are the same as those of the previous sample
of the same thread. In this case, a Stack repeat event is emitted instead of the
frame references of the Python stack. The frames of the sample are then those
of the previous sample of the same thread, followed by the Frame reference
events that come after the Stack repeat event, if any.

In native mode, the native frames that are above the innermost Python frame
might change between samples, even when the Python stack is the same.
Therefore, the trailing native frames of the previous sample must be dropped
before appending the frames that come after the Stack repeat event. Native
frames are those whose file name does not end in `.py` and that are not of the
form `<...>`.


## Asyncio tasks

For applications running on CPython 3.14 and later, Austin reports information
about the asyncio tasks owned by each thread. Asyncio task events are emitted
within the sample of the thread that owns the tasks, between the Stack event and
the frame references of the sample. They are not available in native mode.

Tasks are identified by a sequential number assigned by Austin, and not by their
memory address, so that a new task allocated at the same address as a task that
has gone away is not confused with it. The identifier 0 is never assigned to a
task.

**Task stack** events are emitted for a suspended task only when its position
has changed since the last time it was reported. A task that is executing when
the sample is taken is reported in every such sample, with the portion of the
thread stack that belongs to the task. The event is followed by exactly
*frame_count* Frame reference events that make up the coroutine stack of the
task, from the outermost to the innermost frame; only Frame reference events
count towards *frame_count*. A Task stack event with a *frame_count* of 0
signals that the task has gone away. Task stack events carry no metrics: the
time a task spends at a position is the time of the samples of its thread while
the task is at that position.

**Task waiter** events describe the await relationships between tasks. They are
emitted whenever the set of waiters of a task changes, as a run of consecutive
events with the same *task_id*, one for each direct waiter. The run replaces the
whole set of known waiters of the task. A single event with a *waiter_id* of 0
means that the task has no waiters anymore.

**Running task** events report the task that the thread of the sample is
running. The task is always reported by a Task stack event before it is
referenced. When this event is present, the metrics of the sample belong to the
running task, at its latest reported position, and the frame references of the
sample are those of the thread that are outside the task's own coroutine stack
(e.g. the event loop frames). When the event is absent, the sample belongs to
the thread alone.


## Native frames

When Austin runs in native mode (`-n/--native`, or with the austinp variant),
native frames are reported with Frame events too, interleaved with the Python
frames that they belong to. For a native frame, the file name is the path of the
binary that contains the code (or `native@<address>` if the binary cannot be
determined) and the scope is the name of the function. The location fields are
set to 0, except for the austinp variant, where the line field carries the
offset of the instruction within the function.


## Notes

Reference events, like frame and string events, make use of varint key. To
further reduce the size of MOJO files, these keys are such that their
varint-encoded value is at most 4 bytes, for a total of `2^27` (~134 M) possible
values.

The string key 1 is reserved for the string `<unknown>`, which is used, e.g.,
for the scope of a native frame whose function name cannot be resolved. No
String event is emitted for it.


## What changed

- Added the Stack repeat event.
- Added the Task stack, Task waiter and Running task events for asyncio support.
- The thread identifier of the Stack event is now the name of the thread, when
  it can be resolved.
- The string key 1 is now reserved for the `<unknown>` string.


## Previous versions

- [MOJO v1](https://github.com/P403n1x87/austin/wiki/The-MOJO-File-Format/05162be9c3dc27aa45e26075e4f5139ff8643116)
- [MOJO v2](https://github.com/P403n1x87/austin/wiki/The-MOJO-File-Format/55bf8b2a6720e289d5373cdc20b4b7d967ab7e53)
- [MOJO v3](https://github.com/P403n1x87/austin/wiki/The-MOJO-File-Format/7ada067ace89faf48fd0582fb3546435143aeb11)
