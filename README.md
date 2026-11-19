# ZERO OS (`v0.5.0`) — Spatial Multi-Kernel Compositor & Native WebKit Stream

[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.24355476.svg)](https://doi.org/10.5281/zenodo.24355476)

![ZERO OS v0.5.0 Spatial Multi-Kernel Compositor & Native WebKit Stream Demo](demo_v050.jpg)

**ZERO OS** challenges the modern dogma that integrating a full web browser requires bundling hundreds of megabytes of Chromium bloat and gigabytes of RAM. Guided by a pointer-driven **Virtual Streaming System (VSS)** architecture, `v0.5.0` streams Apple Silicon-native `WebKit` directly into a free-form spatial window alongside an ARM64 Linux kernel (`Virtualization.framework`), a live host macOS PTY shell, bidirectional VirtioFS storage, and a local LLM bridge (`127.0.0.1:11434`)—all within an ~187 KB binary and **~96 MB total host Mach memory footprint**.

## Core Architecture (`v0.5.0`)

* **Zero-Bloat Spatial Web Browser (`WebKit` VSS Viewport)**: Connects the OS-level hardware-accelerated `WKWebView` engine directly to a spatial window descriptor (`WIN_BROWSER`), with dynamic Z-order occlusion handling so Linux and macOS windows naturally overlap the live web stream.
* **Spatial Multi-Kernel Window Compositor**: Five independent, overlapping windows with real-time Z-order focus, title-bar dragging, bottom-right corner resizing (`◢`), and per-window clipping directly over a unified `uint32_t` framebuffer.
* **Hardware Hypervisor & Native Linux Kernel (`Virtualization.framework`)**: Boots an uncompressed ARM64 Linux kernel (`Image` + `initramfs.img`, 2 vCPUs, 256 MB RAM) in ~0.15 seconds alongside a live host `/bin/zsh` PTY window.
* **Bidirectional VirtioFS Direct File Sharing (`VZVirtioFileSystemDeviceConfiguration`)**: Mounts macOS `~/zero_os/shared` directly at `/mnt/macos` inside the guest Linux kernel for zero-latency cross-OS file coexistence.
* **Pointer-Driven `.atom` Spatial Crystallization (`[F6]` / `[F7]`)**: Serializes the entire 5-window spatial state—including window coordinates, Z-order stack, active browser URL, directory path, pixel buffers, and AI replies—into a flat binary `.atom` snapshot in <1 ms.

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
| **Drag Title Bar / Corner `◢`** | Move spatial windows freely or resize dynamically with live reflow |
| **Click Window / `[F1]`** | Bring window to front (Z-order) and route input focus |
| **`[F2]`** or Click URL Bar | Edit Spatial Browser URL (`Cmd+V` to paste URL, `Enter` to navigate) |
| **`[F3]`** | Jump directly to the VirtioFS shared folder (`~/zero_os/shared` <-> `/mnt/macos`) |
| **`[F4]`** | Boot the ARM64 Linux kernel via `Virtualization.framework` |
| **`[F5]`** | Fetch and decode a live JPEG stream via `NSURLSession` + `ImageIO` |
| **`[F6]`** / **`[F7]`** | Crystallize (`state.atom`) or restore 5-window spatial state in <1 ms |
| **`[F8]`** or Click AI Box | Enter local LLM prompt mode (`Cmd+V` paste, `Enter` send, `Cmd+C` copy reply) |
| **`[F9]`** / **`[F10]`** | Trigger 44.1 kHz PCM chime (`[F9]`) or reset 5-window layout (`[F10]`) |
