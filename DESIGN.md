# Design Document — hilda_droid

On-demand extraction of a running app's active notifications via native code
injection.

## 1. Overview

My solution, nicknamed hilda_droid, takes the code injection + payload route. The objective is reading an app's notifications without installing any component, which normally requires a system-level permission; my approach sidesteps that by running code inside the target's own process, where reading its own notifications needs no special grant. An injector built from scratch for aarch64 manipulates the CPU registers via ptrace to load a payload (a .so library) into the target's memory. The payload's init function runs immediately on load via the __attribute__((constructor)) GCC/Clang attribute, executing in-process. Through JNI, the payload calls the framework to read the app's own active notifications, one-shot, extracting the minimum required fields. A Python orchestrator drives the whole flow over adb; the result is written to the app's storage, then read back and printed as NDJSON.

Tests were made with a custom app (included in the repo), Telegram, and the Google Pixel Clock. In all of them, I was able to capture the active notifications.

*Built by me: the injector (ptrace loader), the payload, the ELF symbol resolver, and the Python orchestrator. Reused: the standard NDK/adb toolchain. AI assisted with the Python script, JNI method signatures, document editing and general revision.*


## 2. Architecture

### Components
- main.c — the injector. Attaches to the target with ptrace and loads the payload into its memory.
- payload.c — the payload .so. Runs inside the target process, calls the framework via JNI, and writes the dump.
- address_finder.c — helper functions for resolving memory addresses, used by both the injector and the payload:
    * find_module_base(pid, module_name) — base load address of a module from /proc/<pid>/maps
    * find_dlopen_offset() — offset of the linker's dlopen symbol
    * find_created_vms_offset() — offset of libart's JNI_GetCreatedJavaVMs symbol

- hilda_runner.py — the orchestrator. Drives the flow over adb, then reads, parses, and serializes the result.

Responsibilities are split by layer. The native side does everything that must run inside the target process: VM lookup, JNI calls, and reading the notification objects. The Python side handles orchestration, privilege setup over adb, and converting the raw key=value output into JSON. JSON is assembled in Python rather than C to avoid manual string-building on the native side.


How they work together:

```
[host] Python orchestrator
   |
   |  (1) push binaries, (2) run injector, all over adb
   v
[device] injector (main.c)
   |
   |  ptrace + dlopen: load payload .so into the target process
   v
[device] payload (payload.c) runs in the target's constructor (main thread)
   |
   |  JNI: getActiveNotifications(), then writes the dump
   v
[device] dump file in the app's files dir
   .
   .  (injector returns only after the .so's constructor finishes;
   .   the Python process waited for the injector, so the file is ready)
   .
[host] Python orchestrator reads the dump (adb cat), parses, emits NDJSON
```

## 3. Technique and Step-by-Step Walkthrough

### 3.1 Orchestrator: resolving the target
The Python CLI takes a package name and resolves three things over `adb`: the PID (`pidof`), the app's UID (owner of its data dir), and the app's real data dir (`dumpsys package … | grep dataDir`, which returns `/data/user/0/<pkg>` rather than a hardcoded path).

### 3.2 Staging the payload (root)
The injector and payload are pushed to `/data/local/tmp`. The payload is then copied into the target's own data dir and its ownership fixed with `chown`, see §6 for why this `chown` is load-bearing (it's a SELinux story, not a cosmetic one). Both the payload and the injector are given the right execution permissions and the injector is triggered.

### 3.3 Injection

**3.3.1 ptrace load.** This is the core of the injector: getting the target to load my .so without its cooperation.
The first thing I need is the address of dlopen inside the target. I can't reuse an address from my own process — ASLR maps the linker at a different base in each process. But the offset of a symbol within the linker binary is a fixed property of the file, the same everywhere it's mapped. So I read the dlopen offset straight from the system's linker ELF on disk (/system/bin/linker64, parsed via find_dlopen_offset) and add it to the linker64 base address taken from the target's own /proc/<target_pid>/maps. That gives the real dlopen address in the target's address space, which is passed to the injection routine.
With that address in hand, the rest is two halves — hijacking execution, and setting up the registers so the hijack lands in dlopen with the right arguments.

Using root (which grants access to ptrace), the injector attaches to the target and freezes it. It first saves a full copy of the target's current register state, so the process can be restored to exactly where it was afterward.
Then it rewrites the registers to fake a function call to dlopen in the target's own context. The path to the payload (the app's data dir plus the library name) is written onto the target's stack. The registers are set to match the ARM64 calling convention, which passes the first argument in x0 and the second in x1: x0 is pointed at the stack location holding the payload path, x1 is set to 2 (RTLD_NOW) — the two arguments dlopen expects. The program counter (pc) is pointed at the target's dlopen address, and the link register (lr) is set to 0.
When execution resumes, the pc is inside dlopen, which pulls its arguments straight from x0/x1 and loads the library — running the payload's __attribute__((constructor)) to completion as part of the load. When dlopen returns, the return instruction jumps to whatever is in lr — which I deliberately set to 0, an invalid address. The target takes a segmentation fault trying to execute there. That fault is exactly the signal I want: because the process is under ptrace, the injector catches the SIGSEGV instead of the process dying, which hands control back to the injector at a known point. It then restores the saved registers, detaches, and lets the target continue from where it originally was — now with the payload mapped in and its init already run.
The lr = 0 trick is what makes the whole thing recoverable: rather than trying to compute a safe return address inside the target, I force a fault at a predictable moment and use the debugger relationship to regain control.

### 3.4 Payload

Once loaded, the payload resolves the base address of `libart.so` from `/proc/self/maps`, finds the offset of `JNI_GetCreatedJavaVMs` with the ELF parser from find_created_vms_offset(), computes base + offset, and calls it. That call returns the process's live `JavaVM`. The payload now holds the same VM the app is running on.

**3.4.1 JNIEnv and reading the notifications.** The payload runs on the process's main thread, synchronously, from the `.so`'s `__attribute__((constructor))`. It attaches that thread with `AttachCurrentThread` to obtain a `JNIEnv` — the per-thread handle every JNI call goes through. With that env it walks the framework: gets a `Context` via `ActivityThread.currentApplication()`, asks it for the `NotificationManager`, and calls `getActiveNotifications()`. That returns an array of `StatusBarNotification` — the app's own active notifications, readable without any special permission because the call runs inside the app's own process.

**3.4.2 Fields and group summaries.** For each notification the payload pulls `package_name`, `post_time_ms` (`getPostTime()`), and `title`/`text` from the `Notification.extras` bundle. Notifications carrying `FLAG_GROUP_SUMMARY` (the group envelope, often with null title/text) are flagged `is_group_summary` by testing the flags bit, rather than filtered by a heuristic.

### 3.5 Transport and serialization
The payload writes fields as `key=value` lines (one per line, newlines sanitized) into a temp file in the app's `getFilesDir()`, then atomically renames it to the final name, so the reader never sees a half-written file.. The orchestrator reads the dump (`cat` as root), deletes it, parses the lines, and emits NDJSON on stdout; progress logs go to stderr.

## 4. What Didn't Work

**The listener-push path.** The proper way to receive notifications live is a NotificationListenerService. Since installing one is forbidden, I tried to register one at runtime instead: I loaded a listener class from an in-memory dex and tried to register it through the framework. It consistently failed with a SecurityException demanding the STATUS_BAR_SERVICE permission, which I couldn't grant to the app's UID even as root (pm grant refused it as non-changeable). I chose not to dig further because of the time budget I had. Chasing the listener path problem to its root cause would have been at the expense of a working deliverable.

**Threading, early on.** An earlier design loaded a framework-extending class in the constructor and froze the main thread — verifying that class under the loader lock trips the ANR watchdog. The final `getActiveNotifications` path is light enough to run in the constructor without this problem. A related JNI lesson: a `JNIEnv*` is per-thread and only the `JavaVM*` crosses threads.

## 5. Fidelity & Limitations

**Snapshot, not history.** `getActiveNotifications()` returns only what's active at call time; dismissed notifications are gone.

**Grouping.** The tool captures exactly what the system holds. A test app posting three notifications yields three entries. Telegram collapses a conversation into one active notification: `android.text` carries only the latest line — what the collapsed view shows. The earlier messages live in structured extras (`android.messages` for `MessagingStyle`, `android.textLines` for `InboxStyle`), which the minimal schema doesn't decode. A single-line Telegram result is a faithful capture of the collapsed view, not missing data.

**ABI-bound.** Offsets resolve at runtime (portable across builds), but the binaries and ELF parser assume aarch64/ELF64. Other ABIs need a recompile.

## 6. The Role of Root

**What root enables.**

*ptrace into a foreign process* — the injection itself. Without root (and without sharing the app's UID), attaching to another app's process is denied.

*Cross-UID file staging — the `chown` that isn't cosmetic.* The payload has to live somewhere the target process can `dlopen` it. Root copies it into the app's own data dir:

```
cp /data/local/tmp/libpayload.so /data/data/<pkg>/
chown <app_uid>:<app_uid> /data/data/<pkg>/libpayload.so
```


The subtle part is the second line. Because *root* did the copy, the file lands owned by root. The target, running as the app's UID, cannot load it: SELinux blocks the `dlopen`. The `chown` to the app's own UID aligns the file's ownership with the SELinux policy that governs what an app may load from inside its own data dir, and the load succeeds.

This is worth dwelling on because it's a distinctly *Android* problem. On plain Linux, root owning a file is a non-issue: uid 0 can place a readable library anywhere and any process can load it. Here, root ownership is precisely what breaks the load. It isn't a Unix permission wall (root could read and write freely), it's SELinux enforcing that an app only loads content belonging to its own domain. If this were a DAC/permissions problem, root would have made it disappear; the fact that it didn't is the tell that SELinux, not Unix ownership, is in charge. I found this empirically. The load simply failed until ownership matched.

## 7. Future Work

**Live capture via runtime dex loading.** The most interesting unfinished thread is the listener path from §4. The idea: load a dex in-process (which already works via `InMemoryDexClassLoader`) and have that Java code do the capturing — including recovering the full message fields the native path can't easily reach, by handling notifications on the Java side where the framework APIs are richer. The open question is whether an in-process, non-manifest listener can be authorized at all. I hit a permission wall that root alone didn't open (§4) and chose not to chase it further within the time budget; whether there is a viable path around it — a different registration route, or another authorized bridge — is a genuine research question I'd want to explore with more time.

**Robust transport.** Socket/streaming for continuous output; NDJSON already suits a stream.

