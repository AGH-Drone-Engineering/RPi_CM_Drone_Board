# Stamps the firmware with the commit it was built from. The value goes out over
# the wire as CMDB_ESP_FIRMWARE_BUILD in the GETCONF response (see
# src/node/UartRfBridge.cpp::handleGetconf) and into the boot log, so "which
# commit is on this board" is answerable both from the Pi and over USB.
#
# Mirrors what the host tool does in firmware_CMDB/loracom/makefile, down to the
# only-rewrite-when-changed rule; the comments there explain the reasoning.
#
# Wired in via extra_scripts in platformio.ini as a "pre" script, so the header
# exists and the include path is set before any compile node is created.

Import("env")  # noqa: F821 - injected by SCons

import os
import subprocess

PROJECT_DIR = env.subst("$PROJECT_DIR")  # noqa: F821
BUILD_DIR = env.subst("$BUILD_DIR")  # noqa: F821
VERSION_H = os.path.join(BUILD_DIR, "version.h")
MACRO = "CMDB_ESP_FIRMWARE_BUILD"

# Only this firmware's own sources count as dirty. The marker answers "was this
# image built from exactly the committed sources", and edits elsewhere in the
# repo (the host tool, the KiCad projects, the READMEs) don't change what it was
# built from. Paths are relative to PROJECT_DIR because of the `git -C` below,
# so they cover firmware_IARC_HAT only. scripts/ is in the list for the same
# reason loracom's makefile lists itself: the generator decides what gets baked
# in. The generated header lives under .pio/, which .gitignore covers, so it
# can't report itself dirty.
DIRTY_PATHS = ["src", "lib", "platformio.ini", "scripts"]


def git(*args):
    """Runs git in the project dir; returns its stripped stdout, or None.

    None covers every way this can fail to produce an answer - git not
    installed, the tree not being a checkout (a source export or tarball), a
    corrupt repo, git hanging. None of those may break the build; they only
    make the build "unknown".
    """
    try:
        proc = subprocess.run(
            ["git", "-C", PROJECT_DIR] + list(args),
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=15,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0:
        return None
    return proc.stdout.decode("utf-8", "replace").strip()


def git_version():
    commit = git("rev-parse", "--short=8", "HEAD")
    if not commit:
        return "unknown"
    # --porcelain reports staged, unstaged and untracked changes under the given
    # paths. Any output at all means "not exactly this commit".
    dirty = git("status", "--porcelain", "--", *DIRTY_PATHS)
    return commit + "-dirty" if dirty else commit


version = git_version()
content = '#define %s "%s"\n' % (MACRO, version)

# Rewritten only when the value actually changed, so an untouched mtime keeps
# SCons from recompiling everything that includes this header on every build.
# The flip side - and why this runs unconditionally rather than as a dependency
# of something - is that moving to a new commit without touching a source file
# must still re-stamp the image.
os.makedirs(BUILD_DIR, exist_ok=True)  # PlatformIO creates it, but don't rely on it
try:
    with open(VERSION_H, "r", encoding="utf-8") as f:
        current = f.read()
except OSError:
    current = None
if current != content:
    with open(VERSION_H, "w", encoding="utf-8") as f:
        f.write(content)

print("%s=%s" % (MACRO, version))

# $BUILD_DIR is where the header lands (already gitignored - it's under .pio/),
# so it has to be on the include path. Note what this deliberately is NOT: a -D
# carrying the hash. That would change the compile command line of every object
# - the Arduino core included, since its env is a Clone() of this one - and so
# force a full rebuild on every commit. This -I is a constant path: it changes
# no command line after the first build, and only the translation units that
# actually #include "version.h" get rebuilt, only when the hash changed.
env.Append(CPPPATH=[BUILD_DIR])  # noqa: F821
