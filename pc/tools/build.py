#!/usr/bin/env python3
"""
Build the Animal Crossing PC port (32-bit MinGW on Windows).

This script adds the MinGW32 bin directory to PATH before invoking make,
which is necessary because MinGW GCC writes diagnostics via the Windows
Console API and silently exits when the 32-bit DLLs aren't on PATH.
Running make through this script ensures compiler errors are always visible.

Usage:
    python pc/tools/build.py

Environment variables:
    MINGW32_BIN   Path to mingw32/bin (default: C:\\msys64\\mingw32\\bin)
                  Override if your MSYS2 is installed elsewhere.
"""
import subprocess, os, sys, re

script_dir = os.path.dirname(os.path.abspath(__file__))
pc_dir     = os.path.normpath(os.path.join(script_dir, '..'))
build_dir  = os.path.normpath(os.path.join(pc_dir, 'build32'))
mingw32    = os.environ.get('MINGW32_BIN', r'C:\msys64\mingw32\bin')

env = os.environ.copy()
env['PATH'] = mingw32 + os.pathsep + env.get('PATH', '')

cmake_exe = os.path.join(mingw32, 'cmake.exe')
if not os.path.isfile(cmake_exe):
    cmake_exe = 'cmake'

make_exe = os.path.join(mingw32, 'mingw32-make.exe')
if not os.path.isfile(make_exe):
    make_exe = 'mingw32-make'  # fall back to PATH (Linux/Mac)

def needs_cmake():
    """Return True if cmake must be (re-)run before make."""
    cache = os.path.join(build_dir, 'CMakeCache.txt')
    if not os.path.isdir(build_dir) or not os.path.isfile(cache):
        return True
    # Check that the cached source dir matches our actual pc/ directory
    try:
        with open(cache, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = re.match(r'CMAKE_HOME_DIRECTORY:INTERNAL=(.*)', line.strip())
                if m:
                    cached = os.path.normpath(m.group(1))
                    actual = pc_dir
                    if cached.lower() != actual.lower():
                        print(f"Stale cmake cache detected:")
                        print(f"  cached: {cached}")
                        print(f"  actual: {actual}")
                        print(f"Re-running cmake...")
                        return True
    except OSError:
        return True
    return False

if needs_cmake():
    # Remove only cmake-generated files, preserving bin/ (ROM, saves, settings)
    import shutil
    if os.path.isdir(build_dir):
        print(f"Cleaning stale cmake files in: {build_dir}")
        for entry in os.listdir(build_dir):
            if entry == 'bin':
                continue  # preserve ROM, saves, keybindings, settings
            p = os.path.join(build_dir, entry)
            if os.path.isdir(p):
                shutil.rmtree(p)
            else:
                os.remove(p)
    os.makedirs(build_dir, exist_ok=True)
    # Do NOT pass -DCMAKE_BUILD_TYPE — the project requires GCC's default -O0.
    # Any optimization level (-O1+) exposes UB in decompiled game code and crashes.
    r = subprocess.run(
        [cmake_exe, '..', '-G', 'MinGW Makefiles'],
        cwd=build_dir,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env=env, text=True,
    )
    print(r.stdout)
    if r.returncode != 0:
        print(f"\ncmake failed (exit {r.returncode})")
        sys.exit(r.returncode)
    print("cmake configuration complete.\n")

r = subprocess.run(
    [make_exe],
    cwd=build_dir,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    env=env,
    text=True,
)
out = r.stdout
print(out[-4000:] if len(out) > 4000 else out)
print(f"\nExit: {r.returncode}")
sys.exit(r.returncode)
