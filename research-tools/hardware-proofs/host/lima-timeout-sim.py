#!/usr/bin/env python3
"""Compile the patched Lima recovery functions against ownership/ordering mocks.

Usage: tests/lima-timeout-sim.py <patched Linux source directory>
No GPU, root, kernel module or deliberate host crash is needed. This checks
the actual C functions from the kernel build, not a copy of their algorithm.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def function(source, name):
    start = source.index(name + "(")
    start = source.rfind("\n", 0, start) + 1
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


root = Path(__file__).resolve().parent
source = (Path(sys.argv[1]) / "drivers/gpu/drm/lima/lima_sched.c").read_text()
driver = (Path(sys.argv[1]) / "drivers/gpu/drm/lima/lima_drv.c").read_text()
# DRM_GEM_FOPS does not install fdinfo. Both callbacks must be wired.
assert ".show_fdinfo = drm_show_fdinfo" in driver
assert ".show_fdinfo        = lima_show_fdinfo" in driver
functions = "\n\n".join(function(source, name) for name in (
    "lima_sched_account_done", "lima_sched_start_work", "lima_sched_timedout_job", "lima_sched_pipe_task_done"))
template = (root / "lima_timeout.c").read_text()
with tempfile.TemporaryDirectory(prefix="lima-timeout-", dir=os.getenv("TMPDIR", "/var/tmp")) as temp:
    c = Path(temp) / "test.c"
    exe = Path(temp) / "test"
    c.write_text(template.replace("/* PATCHED_FUNCTIONS */", functions))
    subprocess.run([os.getenv("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-O1", "-g", "-fsanitize=address,undefined",
                    str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
