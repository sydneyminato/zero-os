# ZERO OS (`v0.3.1`) — Native Hardware Hypervisor, ARM64 Linux Kernel & VirtioFS Bridge

[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.24355476.svg)](https://doi.org/10.5281/zenodo.24355476)

**ZERO OS** is an ultra-lightweight (~100 KB binary), dependency-free native spatial operating environment written in C and Objective-C for macOS. In `v0.3.1`, ZERO OS evolves into a hardware-accelerated Virtual Machine Monitor (VMM) powered by Apple `Virtualization.framework`, booting a real ARM64 Linux kernel alongside a live macOS PTY shell with bidirectional VirtioFS file sharing and native local LLM integration.

## Core Architecture (`v0.3.1`)

* **Hardware Hypervisor & Native Linux Kernel Boot (`Virtualization.framework`)**: Allocates 2 vCPUs and 256 MB RAM directly on Apple Silicon hardware to boot an uncompressed ARM64 Linux kernel (`Image` + `initramfs.img`) in ~0.11 seconds without software emulation.
* **Bidirectional VirtioFS Direct File Sharing (`VZVirtioFileSystemDeviceConfiguration`)**: Mounts macOS `~/zero_os/shared` directly at `/mnt/macos` inside the guest Linux kernel for zero-latency cross-OS file exchange.
* **Dual-Kernel Console with Scrollback & Clipboard**: Toggle seamlessly between the Guest Linux Kernel (`aarch64`) and Host macOS Shell (`/bin/zsh`) via `[F1]`, featuring 240-line scrollback history, interactive scrollbars, and clipboard copy/paste (`Cmd+V` / `Shift+Cmd+C`).
* **Native Local LLM Bridge (`NSURLSession`)**: Connects directly to local inference servers (`127.0.0.1:11434`) with interactive scrollbar navigation and `Cmd+C` / `Cmd+V` clipboard support.
* **Sub-Millisecond `.atom` State Crystallization**: Serializes active directory state, terminal buffers, decoded pixel memory, and AI responses into a flat binary `.atom` snapshot (`[F6]` Save / `[F7]` Restore).

## Quick Start (macOS Apple Silicon)

1. Prepare the ARM64 Linux kernel (`~/zero_os/linux/Image` and `initramfs.img`).
2. Compile with virtualization entitlements (`com.apple.security.virtualization`) and launch:

```bash
chmod +x build.sh
./build.sh
```

## Controls

| Key / Action | Function |
| :--- | :--- |
| **`[F1]`** or **`[F1: OS]`** | Switch active console between Guest Linux Kernel and Host macOS Shell |
| **`[F2]`** / **`↑` `↓`** | Navigate and open host POSIX directories |
| **`[F3]`** | Jump directly to the VirtioFS shared folder (`~/zero_os/shared` <-> `/mnt/macos`) |
| **`[F4]`** | Boot the ARM64 Linux kernel via `Virtualization.framework` |
| **`[F5]`** | Fetch and decode a live JPEG stream via `NSURLSession` + `ImageIO` |
| **`[F6]`** / **`[F7]`** | Crystallize (`state.atom`) or restore spatial workspace state in <1 ms |
| **`[F8]`** or Click AI Box | Enter local LLM prompt mode (`Cmd+V` to paste, `Enter` to send, `Cmd+C` to copy reply) |
| **`[F9]`** | Trigger 44.1 kHz direct PCM audio synthesis test |
