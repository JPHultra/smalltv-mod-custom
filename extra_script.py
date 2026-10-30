Import("env")
import os
import shutil
import subprocess

# Windows: SCons's default cmd.exe-based SPAWN can mis-tokenize quoted/long
# build-tool command lines, dropping args. This re-tokenizes and dispatches directly.
PROJECT_DIR = env.subst("$PROJECT_DIR")

# libsodium's port files are compiled into .pio/build/port, outside the env
# build directory, and that directory is shared by every target. After the
# RISC-V C2 build, an Xtensa ESP32 link would otherwise pick up C2 objects and
# fail with "relocations in generic ELF". Clear it once per env.
_port = os.path.join(PROJECT_DIR, ".pio", "build", "port")
if os.path.isdir(_port):
    shutil.rmtree(_port, ignore_errors=True)


def _tokenize(cmdline):
    # \"-escaped quotes (e.g. ESP8266's ARDUINO_BOARD_ID=\"esp12e\") must become a
    # literal " in the token, not get dropped like a bare ".
    tokens = []
    current = []
    in_quotes = False
    i, n = 0, len(cmdline)
    while i < n:
        c = cmdline[i]
        if c == "\\" and i + 1 < n and cmdline[i + 1] == '"':
            current.append('"')
            i += 2
            continue
        if c == '"':
            in_quotes = not in_quotes
        elif c.isspace() and not in_quotes:
            if current:
                tokens.append("".join(current))
                current = []
        else:
            current.append(c)
        i += 1
    if current:
        tokens.append("".join(current))
    return tokens


def _spawn(sh, escape, cmd, args, spawnenv):
    full_env = dict(os.environ)
    full_env.update(spawnenv)
    cmdline = " ".join(str(a) for a in args)
    real_args = _tokenize(cmdline)
    try:
        return subprocess.run(real_args, env=full_env, shell=False, cwd=PROJECT_DIR).returncode
    except OSError:
        return subprocess.run(cmdline, env=full_env, shell=True, cwd=PROJECT_DIR).returncode

# Windows-only: POSIX SCons spawns via sh with single-quote escaping, which
# the double-quote-only tokenizer above would mis-parse.
if os.name == "nt":
    env["SPAWN"] = _spawn
