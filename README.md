# hilda_droid

**Native Android process injection, in-memory DEX execution, and application-context Keystore decryption.**

HILDA is an experimental Android process injector built to explore the complete execution path from low-level AArch64 register manipulation to managed code execution inside a running Android application.

This branch demonstrates how a native injector can establish a JNI/ART bridge, load a Java DEX payload through `InMemoryDexClassLoader`, and reproduce an application's Android Keystore decryption routine **from within the application's own process**.

The demonstration uses a dedicated test application in `test_target/`, which stores encrypted messages in an internal SQLite database.

> **Research scope:** HILDA assumes an already-rooted device. It does not exploit a privilege-escalation vulnerability, bypass Android Keystore's cryptographic protections, or extract raw key material. The demonstration illustrates the security implications of executing code inside a process that is already authorized to use a Keystore key.

## What it does

HILDA combines three components:

1. **Python orchestrator:** coordinates execution from an external host through ADB.
2. **Native C injector and payload:** uses a ptrace-based injection mechanism to load `libpayload.so` into a running AArch64 Android process, then establishes a bridge to ART through JNI.
3. **In-memory DEX payload:** runs application-level Java code inside the target process using `InMemoryDexClassLoader`.

The Keystore demonstration follows this sequence:

1. A dedicated Android test application stores encrypted message records in its internal SQLite database.
2. Its APK is analyzed to identify the Keystore alias, cipher configuration, and decryption routine.
3. The corresponding decryption logic is implemented in `Decrypter.java` and compiled into DEX.
4. The DEX bytes are embedded in `libpayload.so` at build time.
5. HILDA injects the native library into the running test application.
6. The native payload connects to ART through JNI and initializes `InMemoryDexClassLoader`.
7. The managed payload reconstructs the application's decryption operation using its existing Keystore key.
8. The decrypted messages can be collected by the external orchestrator.

The key distinction is that **the key is not exported**. The injected code requests a cryptographic operation within the application's existing execution context, subject to the key's authorization policy.

## Why use an in-memory DEX loader?

The initial HILDA implementation performed Android Framework interactions directly from native C through JNI.

Although functional, that approach required manually resolving classes and methods, handling JNI references and exceptions, and translating application-level operations into numerous native calls.

The DEX loader separates these responsibilities:

- **Native C** handles injection, runtime discovery, and the JNI bridge.
- **Java/DEX** handles application-level operations using familiar Android APIs.

The native `.so` embeds the compiled DEX bytes as a generated C array. At runtime, the payload uses `InMemoryDexClassLoader`, with the application's ClassLoader as its parent, to load the managed code.

This makes the payload logic easier to develop and extend without redesigning the low-level injector.

No additional APK needs to be installed for HILDA itself. The demonstration target application, however, must be installed and running.

### Execution and staging

The injector and native library are initially staged on the device. After initialization, the orchestrator can remove these staging files while the loaded managed code continues to execute within the target process.

This is **memory-resident execution after staging cleanup**, not a claim of complete invisibility or persistence across process restarts. The execution can still leave observable runtime or filesystem artifacts.

## Test scenario: Keystore Oracle

The `test_target/` directory contains a purpose-built Android application used to reproduce an application-context decryption scenario in a controlled environment.

The target stores message contents as encrypted database records and uses Android Keystore to manage the cryptographic key.

HILDA's managed payload reproduces the relevant decryption logic identified during reverse engineering, including the required key alias and cipher configuration.

Because the code executes inside the target process, it operates under the application's UID and can request cryptographic operations permitted for that application's key.

This demonstrates an important distinction:

**Protecting key material from extraction does not, by itself, prevent misuse of authorized cryptographic operations when an application process is compromised.**

The demonstration does not imply that every Keystore key can be used without additional requirements. Key authorization parameters, authentication requirements, and other restrictions still apply.

## Environment (reference / tested)

- **Host:** macOS (Apple Silicon)
- **Device:** Google Pixel 7 Pro (`cheetah`)
- **Android version:** 17
- **Build number:** `CP2A.260705.006`
- **Root:** Magisk 30.7 (Zygisk enabled, Ramdisk: yes)
- **Architecture:** AArch64 / `arm64-v8a`

These are the reference environment details. Compatibility with other Android versions, devices, and configurations has not been established.

## Prerequisites (host)

- **Python 3** (tested with 3.9.6)
- **Android SDK Platform-Tools**, with `adb` available on your `PATH`
- **CMake** (tested with 4.4.3)
- **Android NDK** (tested with 30.0.14904198)
- **Java compiler** (`javac`)
- **Android SDK Build-Tools** providing `d8`
- Android SDK platform corresponding to the build configuration

## Prerequisites (device)

- Rooted Android device with `su` available to the ADB shell
- USB debugging enabled and the host authorized
- AArch64 / `arm64-v8a`
- The demonstration application from `test_target/` installed and running
- Test messages created and encrypted by the target application before running the decryption demonstration

Verify root access:

```bash
adb shell su -c id
```

Expected result includes `uid=0`.

## Build

Building consists of two phases:

1. Compile the managed Java payload into DEX and embed it in a C header.
2. Compile the native injector and shared-library payload.

### Phase 1: Build the DEX payload

Run the following commands from the project root.

```bash
# Point to your Android SDK installation
export ANDROID_HOME="$HOME/Library/Android/sdk"

# 1. Compile Java into JVM bytecode
javac java/Decrypter.java \
  -source 1.8 -target 1.8 \
  -cp $ANDROID_HOME/platforms/android-36/android.jar \
  -d build_java/

# 2. Convert JVM bytecode to DEX
$ANDROID_HOME/build-tools/36.0.0/d8 --min-api 26
--output build_java/ $(find build_java -name "*.class")

# 3. Embed DEX bytes in a C header
cd build_java && xxd -i classes.dex > ../src/msgs_dex.h

# 4. Remove intermediate build files
cd .. && rm -rf build_java/
```

The generated `src/msgs_dex.h` contains the DEX byte array embedded in the native payload.

The native source includes this generated header to make the managed code available at runtime without staging a separate DEX file.

### Phase 2: Build the native binaries

```bash
# Point to your Android NDK installation
export ANDROID_NDK=/path/to/your/ndk

cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-31

cmake --build build
```

This produces:

- `build/hilda_droid` — native process injector
- `build/libpayload.so` — native payload containing the embedded DEX

Both binaries **must** remain inside `build/` because the Python orchestrator expects them there.

## Run

This demonstration is designed for the application included in `test_target/`.

Before execution:

1. Build and install the test APK.
2. Launch the application.
3. Create test messages so that the application stores encrypted records in its internal database.
4. Keep the target process running.
5. Verify that ADB root access works.

Run the orchestrator using the test application's actual package name:

```bash
python3 hilda_runner.py "<test_target_package>" > result.ndjson
```

Replace `<test_target_package>` with the package identifier defined in the test application's Android project.

The orchestrator coordinates injection, payload execution, and retrieval of the demonstration results.

### Expected behavior

A successful run should demonstrate that:

- The native shared library is loaded into the running target process.
- The JNI/ART bridge initializes.
- The embedded DEX payload is loaded through `InMemoryDexClassLoader`.
- The Java decryption routine accesses the application's existing Keystore key.
- Encrypted message records are processed and the resulting plaintext is made available to the demonstration workflow.

The exact output filename, serialization format, and stdout/stderr behavior should be documented according to the current `hilda_runner.py` implementation.

## Troubleshooting

**Root access is unavailable or the script waits for authorization**

Check Magisk root permissions for the ADB shell:

```bash
adb shell su -c id
```

**`adb: command not found` / `FileNotFoundError`**

Ensure Android SDK Platform-Tools are installed and `adb` is available on your `PATH`.

**Injection fails or the target process cannot be found**

Verify that the test application is installed, running, and using the expected package identifier. The injector requires a live target process.

**DEX compilation fails**

Verify that `javac`, the selected Android SDK platform, and `d8` are installed. Also verify that the generated DEX header is included by the native payload.

**DEX class loading or JNI invocation fails**

Inspect the native diagnostic logs. Potential causes include unresolved class names, incompatible method signatures, JNI exceptions, or incorrect initialization of the ART bridge.

**No decrypted messages are produced**

Verify that the test application has generated encrypted messages and that the database, Keystore alias, cipher parameters, and record format match the payload's expectations.

Keystore operations may also fail if the key requires an authorization condition that has not been satisfied.

## Notes / limitations

- **Root is required.** HILDA does not provide root access.
- **AArch64 only.** The native injection mechanism depends on the AArch64 architecture and ELF64.
- **Running process required.** Injection targets a live process.
- **Device-specific assumptions.** Address resolution and other low-level implementation details may depend on the Android build and runtime environment.
- **No Keystore key extraction.** The demonstration requests authorized cryptographic operations rather than exporting raw key material.
- **Key restrictions still apply.** The application UID alone does not override all Keystore authorization requirements.
- **No persistence across process termination.** Managed code loaded in memory does not continue executing after the target process exits.
- **Not undetectable.** The absence of a separately staged DEX file does not imply the absence of runtime or forensic artifacts.
- **Controlled research target.** The included demonstration is intended for the purpose-built test application and authorized research.

## Project layout

```text
hilda_runner.py       Python / ADB orchestrator
java/
  Decrypter.java      Managed decryption payload
src/
  msgs_dex.h          Generated embedded DEX header
  ...                 Native injector and payload sources
build/                 Compiled native binaries
test_target/           Android demonstration application
DESIGN.md              Architecture and implementation notes
```

## Research purpose

HILDA is intended as a low-level Android research project.

Its goal is to make the execution path from native process control to managed Android code explicit, understandable, and extensible.

The Keystore Oracle demonstration illustrates one security consequence of this architecture: executing inside an authorized application process can enable cryptographic operations without exposing the underlying key material.

HILDA is not positioned as a replacement for mature instrumentation frameworks. It provides a narrower, purpose-built foundation for studying Android's execution model, runtime interfaces, and application security boundaries.

Use only on devices and applications you own or are authorized to test.
