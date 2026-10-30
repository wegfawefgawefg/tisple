"""The program race: programs of 100 to 400 lines, written as a programmer would in each
language, rather than the kernels' single loops (races/README.md, "The program race").

Each races/programs/<name>/ has spec.py (PROGRAM: a kernels.Kernel with the buffers, an
input generator and a Python reference), program.c for the three C compilers (other .c
files are other ways to write it) and program.tsp. Every program folds its results into a
u16 checksum at $5F00 the same way in every language; the reference has the same checksum
and the output buffers, and any difference is a wrong result.
"""

import importlib.util
import os

HERE = os.path.dirname(os.path.abspath(__file__))

ORDER = [
    "bytesieve",
    "sort",
    "dhrystone",
    "parser",
    "lz4",
    "vm",
]


def load_one(name):
    path = os.path.join(HERE, "programs", name, "spec.py")
    spec = importlib.util.spec_from_file_location("program_" + name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    program = module.PROGRAM
    program.folder = "programs"
    if program.name != name:
        raise ValueError("%s: spec.py names the program %s" % (path, program.name))
    return program


def load():
    return [load_one(name) for name in ORDER]
