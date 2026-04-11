"""
PlatformIO pre-build script — inject FIRMWARE_BUILD from git commit count.

The build number is the total number of commits reachable from the tip of the
main branch (git rev-list --count origin/main).  This value:
  - is 0 on a fresh clone before any fetching (graceful fallback)
  - strictly increases with every merge / commit to main
  - is the same regardless of which feature branch you build from, because it
    counts only main-branch commits

The value is injected as a C preprocessor define so that config.h does not
need to be edited manually between releases.
"""

import subprocess
Import("env")

def get_build_number():
    # Try origin/main first (reflects the published state), fall back to main,
    # then HEAD, then 0 (e.g. shallow clone or no git at all).
    for ref in ("origin/main", "main", "HEAD"):
        try:
            out = subprocess.check_output(
                ["git", "rev-list", "--count", ref],
                stderr=subprocess.DEVNULL,
                cwd=env.subst("$PROJECT_DIR"),
            )
            return int(out.strip().decode())
        except Exception:
            continue
    return 0

build_num = get_build_number()
print(f"*** FIRMWARE_BUILD = {build_num} (git commit count on main) ***")
env.Append(CPPDEFINES=[("FIRMWARE_BUILD", build_num)])
