# ZERO OS (`v0.4.0`) — Spatial Multi-Kernel Compositor & Virtual Streaming System

[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.24355476.svg)](https://doi.org/10.5281/zenodo.24355476)

![ZERO OS v0.4.0 Spatial Multi-Kernel Compositor Demo](demo_v040.jpg)

**ZERO OS** is an ultra-lightweight (~100 KB binary, ~51 MB runtime footprint) native spatial operating environment written in C and Objective-C for macOS. Built around a pointer-driven **Virtual Streaming System (VSS)** architecture, `v0.4.0` replaces fixed grid layouts with a free-form **Spatial Multi-Kernel Window Compositor**, allowing an ARM64 Linux kernel (`Virtualization.framework`), host macOS PTY shell, VirtioFS shared storage, and a local LLM bridge (`127.0.0.1:11434`) to coexist, overlap, move, and resize in a unified memory space.

## Core Architecture (`v0.4.0`)

* **Spatial Multi-Kernel Window Compositor**: Independent, overlapping windows with dynamic Z-order focus, title-bar dragging, bottom-right corner resizing (`◢`), and per-window clipping directly over a unified `uint32_t` framebuffer.
* **Hardware Hypervisor & Native Linux Kernel (`Virtualization.framework`)**: Boots an uncompressed ARM64 Linux kernel (`Image` + `initramfs.img`, 2 vCPUs, 256 MB RAM) in ~0.13 seconds alongside a live host `/bin/zsh` PTY window.
* **Bidirectional VirtioFS Direct File Sharing (`VZVirtioFileSystemDeviceConfiguration`)**: Mounts macOS `~/zero_os/shared` directly at `/mnt/macos` inside the guest Linux kernel for zero-latency cross-OS file coexistence.
* **Pointer-Driven `.atom` Spatial Crystallization (`[F6]` / `[F7]`)**: Serializes the entire spatial state—including window coordinates, Z-order stack, directory path, pixel buffers, and AI replies—into a flat binary `.atom` snapshot in <1 ms.
* **Native Local LLM Bridge (`NSURLSession`)**: Connects directly to `127.0.0.1:11434` with adaptive text wrapping, scrollbar navigation, and `Cmd+V` / `Cmd+C` clipboard integration.

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
| **Drag Title Bar / Corner `◢`** | Move spatial windows freely or resize dynamically with live text reflow |
| **Click Window / `[F1]`** | Bring window to front (Z-order) and route keyboard/paste input to that OS |
| **`[F2]`** / **`↑` `↓`** | Navigate and open host POSIX directories |
| **`[F3]`** | Jump directly to the VirtioFS shared folder (`~/zero_os/shared` <-> `/mnt/macos`) |
| **`[F4]`** | Boot the ARM64 Linux kernel via `Virtualization.framework` |
| **`[F5]`** | Fetch and decode a live JPEG stream via `NSURLSession` + `ImageIO` |
| **`[F6]`** / **`[F7]`** | Crystallize (`state.atom`) or restore spatial window layout & state in <1 ms |
| **`[F8]`** or Click AI Box | Enter local LLM prompt mode (`Cmd+V` paste, `Enter` send, `Cmd+C` copy reply) |
| **`[F9]`** / **`[F10]`** | Trigger 44.1 kHz PCM chime (`[F9]`) or reset spatial window layout (`[F10]`) |
