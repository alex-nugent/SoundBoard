# Pre-build script (FirmwareSpec.md §16, §20.3): embed the git description as
# FW_VERSION. `--always` makes it work before the first tag (short hash).
import subprocess

Import("env")


def describe():
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--dirty", "--always"],
            cwd=env.subst("$PROJECT_DIR"),
            stderr=subprocess.DEVNULL,
        )
        return out.decode().strip() or "unknown"
    except Exception:  # not a git checkout, or git missing
        return "unknown"


version = describe()
env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])
print("FW_VERSION = %s" % version)
