"""Advisory file lock shared by the daemon, refresh paths and setup UI."""
from __future__ import annotations

import contextlib
import os
import time

try:
    import fcntl
except ImportError:  # Windows: no advisory lock; atomic replace still applies.
    fcntl = None


@contextlib.contextmanager
def file_lock(path, timeout=10.0):
    """Hold an exclusive lock on path.lock, waiting up to timeout seconds."""
    if fcntl is None:
        yield
        return
    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        deadline = time.time() + timeout
        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.time() >= deadline:
                    raise TimeoutError("timed out waiting for the credentials lock")
                time.sleep(0.1)
        yield
    finally:
        fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)
