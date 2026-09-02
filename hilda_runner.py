#!/usr/bin/env python3
"""
hilda_runner.py — deploys native binaries to an Android device and runs the
notification dump. Input: the target package name.

Output contract:
  - stdout: ONLY the NDJSON result (one notification object per line)
  - stderr: all progress logs and diagnostics

Flow:
  1. resolve target PID from package name
  2. resolve target UID from package name
  3. resolve the app's real data dir (no hardcoded /data/data/<pkg>)
  4. push native binaries (injector + .so) to the device
  5. run a root shell command sequence (payload prep + injection)
  6. read the dump file, parse it, emit NDJSON
"""

import subprocess
import sys
import json

# ---- configuration ---------------------------------------------------------
DEVICE_TMP = "/data/local/tmp"                  # staging dir on device
INJECTOR_LOCAL = "./build/hilda_droid"          # local path to compiled injector
LIB_LOCAL = "./build/libpayload.so"             # local path to compiled .so
INJECTOR_REMOTE = f"{DEVICE_TMP}/hilda_droid"
LIB_REMOTE = f"{DEVICE_TMP}/libpayload.so"
DUMP_NAME = "notif_dump.txt"                     # written by the payload into <data_dir>/files/


# ---- logging ---------------------------------------------------------------
def log(msg):
    """Progress logs go to stderr, keeping stdout clean for the NDJSON result."""
    print(msg, file=sys.stderr)


# ---- adb helpers -----------------------------------------------------------
def run(cmd, check=True):
    """Run a local command (list form). Returns CompletedProcess. Logs to stderr."""
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.stdout.strip():
        log(f"[stdout]\n{result.stdout.rstrip()}")
    if result.stderr.strip():
        log(f"[stderr]\n{result.stderr.rstrip()}")
    if check and result.returncode != 0:
        log(f"[-] command failed (rc={result.returncode})")
        sys.exit(1)
    return result


def adb(*args, check=True):
    """Wrapper for `adb ...`."""
    return run(["adb", *args], check=check)


def adb_shell_root(command_str, check=True):
    """Run a command (or && chain) as root via su -c."""
    log(f"[*] ROOT SHELL: {command_str}")
    return adb("shell", "su", "-c", command_str, check=check)


# ---- steps -----------------------------------------------------------------
def resolve_pid(package):
    """Get the PID of the running target app."""
    log(f"[*] Resolving PID for package: {package}")
    result = adb("shell", "pidof", package, check=False)
    pid = result.stdout.strip()
    if not pid:
        log(f"[-] Package '{package}' is not running. Open the app first.")
        sys.exit(1)
    # pidof may return several PIDs separated by spaces; take the first.
    pid = pid.split()[0]
    log(f"[+] Target PID: {pid}")
    return pid


def resolve_uid(package, data_dir):
    """Get the app's numeric UID (e.g. 10325) from its data dir owner."""
    log(f"[*] Resolving UID for package: {package}")
    result = adb_shell_root(f"stat -c '%u' {data_dir}")
    uid = result.stdout.strip()
    if not uid.isdigit():
        log(f"[-] Could not resolve UID (got: '{uid}')")
        sys.exit(1)
    log(f"[+] Target UID: {uid}")
    return uid


def resolve_data_dir(package):
    """Ask the system for the app's data dir instead of hardcoding /data/data/<pkg>."""
    log(f"[*] Resolving data dir for package: {package}")
    # dumpsys reports the app's real dataDir (e.g. /data/user/0/<pkg>)
    result = adb_shell_root(f"dumpsys package {package} | grep dataDir")
    line = result.stdout.strip()
    if "dataDir=" in line:
        data_dir = line.split("dataDir=", 1)[1].split()[0].strip()
        log(f"[+] Data dir: {data_dir}")
        return data_dir
    log("[-] Could not resolve data dir")
    sys.exit(1)


def push_binaries():
    """Push injector + .so to the device staging dir."""
    log("[*] Pushing native binaries to device...")
    adb("push", INJECTOR_LOCAL, INJECTOR_REMOTE)
    adb("push", LIB_LOCAL, LIB_REMOTE)
    log("[+] Binaries staged.")


def run_root_script(commands):
    """
    Run a sequence of commands inside a SINGLE root shell — exactly like
    typing `su` then pasting the commands. Feeds the script via stdin.
    """
    script = "\n".join(commands) + "\n"
    log("[*] ROOT SCRIPT:")
    for c in commands:
        log(f"    {c}")

    # `adb shell su` opens a root shell; we pipe the commands into its stdin.
    result = subprocess.run(
        ["adb", "shell", "su"],
        input=script,
        capture_output=True,
        text=True,
    )
    if result.stdout.strip():
        log(f"[stdout]\n{result.stdout.rstrip()}")
    if result.stderr.strip():
        log(f"[stderr]\n{result.stderr.rstrip()}")
    return result


def run_injection(pid, package, uid_name, data_dir):
    """Reproduce the exact manual root sequence, in one root shell."""
    log("[*] Running injection + dump sequence...")

    payload = f"{data_dir}/libpayload.so"

    commands = [
        f"rm -f {payload}",
        f"ls -l {data_dir}/",
        f"cp {LIB_REMOTE} {data_dir}",
        f"ls -l {data_dir}/",
        f"chown {uid_name}:{uid_name} {payload}",
        f"ls -l {data_dir}/",
        f"chmod 777 {payload}",
        f"chmod +x {INJECTOR_REMOTE}",
        f"{INJECTOR_REMOTE} {pid} {package}",
    ]

    run_root_script(commands)
    log("[+] Injection sequence complete.")


def collect_dump(data_dir):
    """Read the dump file written by the payload, then remove it."""
    path = f"{data_dir}/files/{DUMP_NAME}"
    log(f"[*] Reading dump from {path}")
    result = adb_shell_root(f"cat {path}")
    dump_text = result.stdout
    # security: remove the file from the device after collecting
    adb_shell_root(f"rm -f {path}")
    return dump_text


# ---- parsing ---------------------------------------------------------------
def parse_dump(dump_text):
    """
    Parse the key=value dump into a list of notification dicts.

    Line grammar (one field per line):
        dump_count=<n>
        notif.<i>.<field>=<value>

    Only the minimal fields are kept in the output:
        package_name, post_time_ms, title, text
    plus is_group_summary (used to identify group-summary notifications).
    """
    notifs = {}   # keyed by index so lines can arrive in any order

    def get_notif(i):
        if i not in notifs:
            notifs[i] = {
                "package_name": None,
                "post_time_ms": None,
                "title": None,
                "text": None,
                "is_group_summary": None,
            }
        return notifs[i]

    for line in dump_text.splitlines():
        line = line.strip()
        if not line or "=" not in line:
            continue

        key, value = line.split("=", 1)   # split on FIRST '=' only

        if key == "dump_count":
            continue

        parts = key.split(".")
        if parts[0] != "notif" or len(parts) < 3:
            continue

        try:
            idx = int(parts[1])
        except ValueError:
            continue

        n = get_notif(idx)
        field = parts[2]

        if field == "post_time_ms":
            n["post_time_ms"] = int(value) if value.isdigit() else value
        elif field == "is_group_summary":
            n["is_group_summary"] = (value.lower() == "true")
        elif field in ("package_name", "title", "text"):
            n[field] = value
        # any other field (id, extra.*, action.*) is ignored for the minimal output

    # finalize: return notifications sorted by index
    return [notifs[i] for i in sorted(notifs.keys())]


def dump_to_ndjson(dump_text):
    """
    Serialize parsed notifications as NDJSON (one JSON object per line).
    Preferred for streams: each line is an independent, self-contained record.
    """
    notifs = parse_dump(dump_text)
    lines = []
    for n in notifs:
        lines.append(json.dumps(n, ensure_ascii=False, separators=(",", ":")))
    return "\n".join(lines)


# ---- main ------------------------------------------------------------------
def main():
    if len(sys.argv) != 2:
        log(f"Usage: {sys.argv[0]} <package.name>")
        sys.exit(1)

    package = sys.argv[1]
    log(f"=== hilda_runner starting for '{package}' ===")

    # sanity: is a device connected?
    adb("get-state")

    pid = resolve_pid(package)
    data_dir = resolve_data_dir(package)
    uid = resolve_uid(package, data_dir)
    push_binaries()
    run_injection(pid, package, uid, data_dir)
    dump_text = collect_dump(data_dir)

    # the ONLY thing written to stdout is the NDJSON result:
    print(dump_to_ndjson(dump_text))


if __name__ == "__main__":
    main()