#!/usr/bin/env python3
"""Generate main/wifi_credentials.h from the app's .env.

Run automatically at CMake configure time (see main/CMakeLists.txt). Both .env
and the generated header are gitignored — secrets never enter the repo.

.env format (KEY=VALUE, one per line, value is the rest of the line verbatim):

    WIFI_SSID=4PrivetDrive
    WIFI_PASSWORD=your-password-here
    API_TOKEN=                # blank => API is open on the LAN

Usage: gen_credentials.py <env_path> <header_path>
"""

import sys


def load_env(path):
    env = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line or line.lstrip().startswith("#") or "=" not in line:
                continue
            key, _, val = line.partition("=")
            env[key.strip()] = val  # value kept verbatim (passwords may have spaces)
    return env


def c_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    env_path, header_path = argv[1], argv[2]

    try:
        env = load_env(env_path)
    except FileNotFoundError:
        sys.exit(
            f"\n{env_path} not found.\n"
            "Create it (see tools/gen_credentials.py header) with WIFI_SSID,\n"
            "WIFI_PASSWORD and optional API_TOKEN, then rebuild.\n"
        )

    ssid = env.get("WIFI_SSID", "")
    if not ssid:
        sys.exit(f"{env_path}: WIFI_SSID is required")
    password = env.get("WIFI_PASSWORD", "")
    token = env.get("API_TOKEN", "")

    with open(header_path, "w", encoding="utf-8") as fh:
        fh.write(
            "// GENERATED from .env by tools/gen_credentials.py — do not edit or commit.\n"
            "#pragma once\n"
            f'#define WIFI_SSID "{c_escape(ssid)}"\n'
            f'#define WIFI_PASSWORD "{c_escape(password)}"\n'
            f'#define API_TOKEN "{c_escape(token)}"\n'
        )
    state = "open (no token)" if not token else "token required"
    print(f"gen_credentials: SSID={ssid!r}, API {state} -> {header_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
