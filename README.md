# ZERO OS (`v0.2.0`) — Universal Native Spatial Compositor & Live PTY Kernel

[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.24355476.svg)]([https://doi.org/10.5281/zenodo.24355476](https://doi.org/10.5281/zenodo.24355476))

![ZERO OS v0.2.0 Intel x86_64 Native](demo_x86_64.jpg)

A dependency-free, single-file C/Objective-C spatial workspace and lightweight system compositor compiled as a **124 KB Universal Binary (`arm64` + `x86_64`)** for both Apple Silicon and legacy Intel Macs.

---

## Verified Architecture & Real Measurements (No Mockups)

Every subsystem in `v0.2.0` executes directly against native OS kernels and system frameworks with **zero external wrapper scripts (no Python, no Electron, no third-party libraries)**:

* **Universal Binary Footprint:** `124,464 bytes` (`Mach-O universal binary` supporting both `arm64` and `x86_64`).
* **Real Process Memory (Mach Resident RAM):**
  * **Intel Mac (`x86_64`):** `~30.1 MB`
  * **Apple Silicon (`arm64`):** `~45.0 – 50.4 MB`
* **4 Integrated Live Subsystems:**
  1. **Host Filesystem (`opendir` / `stat`):** Direct POSIX directory traversal and real-time file inspection.
  2. **Live UNIX Shell (`forkpty`):** Real interactive `/bin/zsh` pseudo-terminal rendered directly into the unified pixel buffer (`uname`, `ls`, `top`, compilers, and network tools execute live).
  3. **Live Web Stream & `.atom` State Crystallization:** Native `NSURLSession` + `ImageIO` JPEG decoding into unified memory, with sub-millisecond binary snapshot serialization (`state.atom`).
  4. **Kernel Telemetry & Native Local AI Bridge:** Real-time `task_info` Mach memory reporting, `44.1 kHz` mathematical PCM audio synthesis (`AudioToolbox`), and direct `NSURLSession` HTTP/JSON communication with local LLM endpoints (`127.0.0.1:11434`).

---

## Build & Run (Apple Silicon & Intel Mac)

Requires only standard macOS Command Line Tools (`clang`):

```bash
chmod +x build.sh
./build.sh
```

To explicitly verify `x86_64` (Intel Mac) execution on Apple Silicon via Rosetta 2:

```bash
arch -x86_64 ./zero_os
```

---

## Roadmap

* **`v0.1.0` (Archived):** Initial UI concept and defensive prior-art publication (`DOI: 10.5281/zenodo.24355476`).
* **`v0.2.0` (Current):** Replaced all UI mockups with live POSIX `forkpty()` shell, real filesystem integration, native `NSURLSession` networking/LLM bridge, and Universal `arm64`/`x86_64` binary support (~30 MB RAM on `x86_64`).
* **`v0.3.0` (Next):** Direct ARM64/x86_64 Linux kernel boot integration via Apple `Virtualization.framework` / `Hypervisor.framework`.

---

## License

Apache License 2.0 — Copyright (c) 2026 `sydneyminato` / ZERO OS Project.
