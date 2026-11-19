# ZERO OS (`v0.6.0`) — Autonomous AI Intent Engine & Multi-Kernel Spatial Compositor

[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.24355476.svg)](https://doi.org/10.5281/zenodo.24355476)

![ZERO OS v0.6.0 Autonomous AI Intent Engine Demo](demo_v060.jpg)

**ZERO OS** unifies an ARM64 Linux kernel (`Virtualization.framework`), a live host macOS PTY shell, bidirectional VirtioFS storage, an Apple Silicon-native `WebKit` browser stream, and an **Autonomous Local AI Intent Engine (`127.0.0.1:11434`)** inside a single pointer-driven **Virtual Streaming System (VSS)**—running within an ~187 KB binary and **~80 MB total host Mach memory footprint**.

## What is New in `v0.6.0`

* **Autonomous Cross-OS AI Execution (`[LINUX]` / `[MACOS]` / `[WEB]`)**: Translates natural-language instructions (Japanese/English) into executable scripts dispatched automatically across the guest Linux kernel via VirtioFS (`/mnt/macos/ai_task.sh`), the host macOS shell (`~/zero_os/shared/ai_macos.sh`), and the live spatial `WebKit` browser.
* **Automatic Cross-OS Path Normalization**: Rewrites host/guest path mismatches (`~/zero_os/shared` <-> `/mnt/macos`) in real time inside the C bridge so compact local LLMs execute cross-kernel pipelines reliably.
* **AppKit-Synchronized Overlay Pipeline**: Decouples subview frame synchronization from `drawRect:` rendering for rock-solid 30 fps spatial window compositing alongside hardware-accelerated web media.

## Core Architecture

* **Spatial Multi-Kernel Window Compositor**: Five independent, overlapping windows with dynamic Z-order focus, title-bar dragging, bottom-right corner resizing (`◢`), and per-window clipping directly over a unified `uint32_t` framebuffer.
* **Zero-Bloat Spatial Web Browser (`WebKit` VSS Viewport)**: Hardware-accelerated `WKWebView` synchronized to `WIN_BROWSER` with automatic Z-order occlusion snapshots.
* **Hardware Hypervisor & VirtioFS (`Virtualization.framework`)**: Boots an uncompressed ARM64 Linux kernel (`Image` + `initramfs.img`, 2 vCPUs, 256 MB RAM) in ~0.15 seconds with zero-latency file coexistence (`~/zero_os/shared` <-> `/mnt/macos`).
* **Pointer-Driven `.atom` Spatial Crystallization (`[F6]` / `[F7]`)**: Serializes the entire 5-window spatial state—including window coordinates, Z-order stack, active browser URL, directory path, pixel buffers, and AI state—into a flat binary `.atom` snapshot in <1 ms.

## Controls

| Key / Action | Function |
| :--- | :--- |
| **`[F8]`** or Click AI Box | Enter Autonomous AI Intent mode (`Cmd+V` paste instruction, `Enter` execute) |
| **`[F11]`** | Toggle AI Autonomous Execution (`AUTO: ON` / `Plan Only`) |
| **Drag Title Bar / Corner `◢`** | Move spatial windows freely or resize dynamically with live reflow |
| **Click Window / `[F1]`** | Bring window to front (Z-order) and route input focus |
| **`[F2]`** or Click URL Bar | Edit Spatial Browser URL (`Cmd+V` to paste URL, `Enter` to navigate) |
| **`[F3]`** | Jump directly to the VirtioFS shared folder (`~/zero_os/shared` <-> `/mnt/macos`) |
| **`[F4]`** / **`[F5]`** | Boot ARM64 Linux kernel (`[F4]`) / Fetch live JPEG stream (`[F5]`) |
| **`[F6]`** / **`[F7]`** | Crystallize (`state.atom`) or restore 5-window spatial state in <1 ms |
| **`[F9]`** / **`[F10]`** | Trigger 44.1 kHz PCM chime (`[F9]`) or reset 5-window layout (`[F10]`) |
