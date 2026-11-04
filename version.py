# PlatformIO pre-build script: compiles the VERSION file into the firmware as
# DIAL_VERSION, so the dial can say which release it is running.
Import("env")  # noqa: F821  (provided by PlatformIO)
import os

path = os.path.join(env.subst("$PROJECT_DIR"), "VERSION")  # noqa: F821
try:
    version = open(path).read().strip()
except OSError:
    version = "dev"
env.Append(CPPDEFINES=[("DIAL_VERSION", '\\"%s\\"' % version)])  # noqa: F821
