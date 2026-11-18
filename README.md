# hilda_droid

On-demand dump of a running Android app's active notifications, via native
`.so` injection and JNI — with no installed-APK component. Outputs NDJSON.

> **Note:** This branch (`feat/dex-loader-notifications`) introduces an advanced payload architecture using `InMemoryDexClassLoader` to execute the core logic in Kotlin instead of pure C.

## What it does

Injects a native library into a running target process (ptrace-based loader,
resolving linker64/libart symbols at runtime). 

**The DEX Loader Approach:** Instead of wrestling with hundreds of lines of pure JNI C to call Android APIs (managing local references, checking for exceptions at every step, and manually parsing `Bundle` extras), this branch shifts the notification-dumping logic to Kotlin. 

The native .so payload now acts exclusively as a springboard. It carries an embedded .dex payload inside its data section (converted via xxd) and uses InMemoryDexClassLoader to instantiate the Kotlin classes directly inside the target's ART. This provides full, native access to the Android Framework API without the friction of raw JNI, while maintaining a strictly fileless JVM execution footprint. No .apk, .dex, or optimized .odex/.vdex caches are ever dropped to disk. The native .so can be unlinked immediately after injection, leaving the Kotlin payload executing entirely in memory.3

A Python orchestrator drives the whole flow over adb.

## Environment (reference / tested)

- **Host:** macOS (Apple Silicon)
- **Device:** Google Pixel 7 Pro (codename "cheetah")
- **Android version:** 17
- **Build number:** CP2A.260705.006
- **Root:** Magisk 30.7 (Zygisk: enabled, Ramdisk: yes)

## Prerequisites (host)

- **Python 3** (tested with Python 3.9.6)
- **Android SDK platform-tools**, with `adb` available on your `PATH`.
- **CMake** (tested with version 4.4.3)
- **Android NDK** (tested with 30.0.14904198)
- **Kotlin Compiler** (`kotlinc`) and **Android SDK Build-Tools** (`d8`) for compiling the DEX payload.

## Prerequisites (device)

- **Rooted with Magisk**, with `su` granting root to the shell. Verify with
  `adb shell su -c id` (must print `uid=0`).
- **USB debugging** enabled and the host authorized.
- **ABI:** arm64-v8a (aarch64)

## Build

Building is split into two phases: compiling the Kotlin payload into a C header, and compiling the native binaries.

### Phase 1: Build the Kotlin Payload (DEX Header)

Before compiling the C injector, you must compile the Kotlin source into a DEX array embedded in a C header. Run these commands from the **project root**:

```bash
# Point this to your Android SDK install
export ANDROID_HOME=$HOME/Library/Android/sdk

# 1. Compile Kotlin to JVM bytecode
kotlinc kotlin/NotificationDumper.kt \
  -jvm-target 1.8 \
  -classpath $ANDROID_HOME/platforms/android-36/android.jar \
  -d build_kt/

# 2. Convert bytecode to a single DEX file
$ANDROID_HOME/build-tools/36.0.0/d8 --min-api 26 --output build_kt/ $(find build_kt -name "*.class")

# 3. Generate the C header (subshell preserves the 'classes_dex' variable name)
cd build_kt && xxd -i classes.dex > ../src/notif_dex.h

# 4. Clean up
rm -rf build_kt/
```

### Phase 2: Build the Native Binaries

Now compile the injector and the payload (which will `#include "notif_dex.h"` internally).

```bash
# Point this at YOUR NDK install
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

Without the redirection, the NDJSON prints to your terminal alongside the logs.

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
- aarch64/ELF64 only; injector is dependent on aarch64 processor architecture.

## Layout

- `hilda_runner.py` — orchestrator
- `kotlin/` — high-level Android payload logic
- `src/` — native sources (injector, payload, and generated `notif_dex.h`)
- `build/` — compiled binaries
- `samples/` — real captured output
- `test_target/` — demo app used to generate notifications (not part of the solution, just a simple target)
- `DESIGN.md` — design document
