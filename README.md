# hilda_droid

On-demand dump of a running Android app's active notifications, via native
`.so` injection and JNI — with no installed-APK component. Outputs NDJSON.

## What it does

Injects a native library into a running target process (ptrace-based loader,
resolving linker64/libart symbols at runtime), then calls `getActiveNotifications()`
in-process via JNI to read the app's own active notifications. A Python orchestrator
drives the whole flow over adb.

## Environment (reference / tested)

- **Host:** macOS (Apple Silicon)
- **Device:** Google Pixel 7 Pro (codename "cheetah")
- **Android version:** 17
- **Build number:** CP2A.260705.006
- **Root:** Magisk 30.7 (Zygisk: enabled, Ramdisk: yes)

## Prerequisites (host)

- **Python 3** (tested with Python 3.9.6)
- **Android SDK platform-tools**, with `adb` available on your `PATH`.
  The orchestrator invokes `adb` as a bare command, so it must be resolvable
  from your shell. Verify with:
```bash
  adb version
```
- **CMake** (tested with version 4.4.3)
- **Android NDK** (tested with 30.0.14904198)

## Prerequisites (device)

- **Rooted with Magisk**, with `su` granting root to the shell. Verify with
  `adb shell su -c id` (must print `uid=0`).
  
- **USB debugging** enabled and the host authorized.
- **ABI:** arm64-v8a (aarch64)

## Build

```bash
# Point this at YOUR NDK install. Example (macOS):
# /Users/username/Library/Android/sdk/ndk/30.0.14904198
export ANDROID_NDK=/path/to/your/ndk 
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-31
cmake --build build
```

This produces `build/hilda_droid` (injector) and `build/libpayload.so` (payload).
Both binaries **must** stay inside the `build/` folder, that's where the Python
orchestrator looks for them.


## Run

The target app must be **running** and have at least one active notification to
produce results.

Invoke the script with `python3`, passing the target package name as the only
argument. The `>` redirects stdout to a file, where you can read the NDJSON
output.

Example:

```bash
python3 hilda_runner.py "org.telegram.messenger.web" > result.ndjson
```

- **stdout:** NDJSON result (one notification per line)
- **stderr:** progress logs and diagnostics


## Output

By default the result (NDJSON) is written to **stdout** and progress logs to
**stderr**. Redirecting stdout with `>` therefore captures a clean result file
with no logs mixed in:

```bash
python3 hilda_runner.py <package> > result.ndjson
```

Without the redirection, the NDJSON prints to your terminal alogside the logs.

Output example:

```json
{"package_name":"...","post_time_ms":...,"title":"...","text":"...","is_group_summary":false}
```

See `samples/` for real captured output.

## Troubleshooting

- **Script hangs during injection:** the root shell is likely waiting for a
  Magisk confirmation. Grant root to the shell context in Magisk and retry.
  Confirm with `adb shell su -c id` (must print `uid=0` with no prompt).
- **`adb: command not found` / `FileNotFoundError`:** `adb` is not on your PATH.
  See host prerequisites.
- **Empty output:** the target app has no active notifications right now.
  Trigger one in the app and re-run.
- **`ptrace attach failed` / injection error:** the target app is not running.
  ptrace can only attach to a live process — launch the app first. 

## Notes / limitations

- Captures a **snapshot** of currently-active notifications (not history).
- Requires root (see DESIGN.md for why, and what root does/doesn't grant).
- aarch64/ELF64 only; injector is dependent on aarch64 processor architecture

## Layout

- `hilda_runner.py` — orchestrator
- `src/` — native sources (injector, payload)
- `build/` — compiled binaries
- `samples/` — real captured output
- `test_target/` — demo app used to generate notifications (not part of the solution, just a simple target)
- `DESIGN.md` — design document
