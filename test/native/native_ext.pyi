from typing import final

@final
class ThreadGroup:
    """Opaque token returned by start_threads().  Pass to join_threads()."""

    ...

def start_threads(n: int) -> ThreadGroup:
    """Spawn *n* OS threads named ``Native-<i>`` with no PyThreadState.

    Returns a token to pass to :func:`join_threads`.
    """
    ...

def join_threads(token: ThreadGroup) -> None:
    """Signal all threads in the group to stop and wait for them to finish.

    Raises :exc:`RuntimeError` if the token has already been consumed.
    """
    ...
