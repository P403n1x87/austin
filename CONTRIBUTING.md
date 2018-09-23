# Contributing to Austin

Thanks for taking the time to contribute or considering doing so.

The following is a set of guidelines for contributing to Austin.

## Preamble

Presently, Austin is mainly a C project. The reason behind this is because
CPython is written in C, and therefore this is the most natural choice of
language for anything that needs to interface to its ABI.

## The Coding Style

The Python C API is written in an Object Oriented style. Austin adopts this
style too. Some translation unit are used to export a main "object". For
example, the `py_code.c` source exports the type `py_code_t`, which is Austin's
representation of the analogous `PyCodeObject` from the Python C API.

Utility units, like `mem.c` or `logging.c` need not export objects, but may
implement a singleton pattern.

### Conventions

Every object is a structure that takes a name of the form `<object>_t`, and is
declared in a pair of sources `object.c` and `object.h`.

#### Methods

Methods should have a general structure that includes the object name as a
prefix, and the rest must give a short description of what the method does. If
the method is a _class_ method, then the prefix and the method description is
separated by only one underscore `_`. For instance method, the separator is a
double underscore `__`.

Private methods, i.e. those that are used only within the translation unit,
must be `static` and prefixed with an underscore `_` so that they can be easly
recognised in the code.

To summarise, here is an example of the above rules in place for an object of
type `object_t`.

| Method Type             | Example                   |
| ----------------------- | ------------------------- |
| Private class method    | `_object_get_version`     |
| Private instance method | `_object__get_rgba_value` |
| Public class method     | `object_new`              |
| Public instance method  | `object__get_color`       |


#### Objects Life-cycle

Every translation unit that exports an object type that can be instantiated at
run-time must provide at least two public methods: the class method
`<object>_new` for object creation and the instance method `<object>__destroy`
for object disposal. There isn't enough complexity at the moment to justify the
use of smart pointers, so objects are passed around by pointers, and care should
be taken to destroy all the objects no longer needed to avoid memory leaks.

Please make sure that your changes do not introduce any memory leaks before
submitting a new PR.

### Dependencies Between Units

Circular dependencies among translation units should be avoided as much as
possible. Whilst sources are stored in a flat file system structure, their
dependency tree should be a DAG.


## Opening PRs

Everybody is more than welcome to open a PR to fix a bug/propose enhancements/
implement missing features. If you do, please adhere to the following
styleguides as much as possible.


### Git Commit Messages

This styleguide is taken from the Atom project.

* Use the present tense ("Add feature" not "Added feature")
* Use the imperative mood ("Move cursor to..." not "Moves cursor to...")
* Limit the first line to 72 characters or less
* Reference issues and pull requests liberally after the first line


### Labels

When opening a new PR, please apply a label to them. Try to use existing labels
as much as possible and only create a new one if the current ones are not
applicable.
