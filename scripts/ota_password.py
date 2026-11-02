import re
from pathlib import Path

Import("env")

if env.subst("$PIOENV") == "esp32dev_ota":
    secrets_file = Path(env["PROJECT_DIR"]) / "include" / "secrets.h"
    if not secrets_file.is_file():
        raise RuntimeError(
            "OTA upload requires include/secrets.h with a non-empty OTA_PASSWORD."
        )

    match = re.search(
        r'^\s*#define\s+OTA_PASSWORD\s+"([^"]*)"\s*$',
        secrets_file.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    if match is None or not match.group(1):
        raise RuntimeError(
            "OTA upload requires a non-empty OTA_PASSWORD in include/secrets.h."
        )

    env.Replace(UPLOAD_FLAGS=["--auth=" + match.group(1)])
