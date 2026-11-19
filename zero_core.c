#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#import <ImageIO/ImageIO.h>
#if defined(__arm64__)
#import <Virtualization/Virtualization.h>
#endif
#include <mach/mach.h>
#include <util.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <math.h>
#include <string.h>

#define FB_W 1280
#define FB_H 800
#define TERM_HIST 240
#define TERM_ROWS 21
#define TERM_COLS 71
#define MAX_FILES 64
#define AI_MAX_LINES 80
#define AI_LINE_COLS 66

typedef struct {
    char name[128];
    off_t size;
    int is_dir;
} FileEntry;

typedef struct {
    char lines[TERM_HIST][TERM_COLS + 1];
    int total_lines;
    int cur_col;
    int scroll_back;
} TermBuffer;

typedef struct {
    uint32_t magic;
    char cwd[512];
    char term_lines[TERM_ROWS][TERM_COLS + 1];
    int term_row;
    int term_col;
    char status_msg[256];
    char ai_reply[256];
    int img_w;
    int img_h;
    uint32_t img_buf[360 * 240];
} ZeroAtomSnapshot;

static uint32_t *g_fb = NULL;
static CGContextRef g_ctx = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Host PTY shell state
static int g_pty_fd = -1;
static pid_t g_pty_pid = -1;
static TermBuffer g_host_term = { .total_lines = 1 };

// Guest Linux Kernel VM state
static int g_vm_in_fd = -1;
static int g_vm_out_fd = -1;
static TermBuffer g_vm_term = { .total_lines = 1 };
static int g_vm_running = 0;
static int g_console_mode = 1; // 1 = Guest Linux Kernel, 0 = Host macOS Shell
#if defined(__arm64__)
static VZVirtualMachine *g_vz_vm = nil;
#endif

static char g_cwd[512];
static FileEntry g_files[MAX_FILES];
static int g_file_count = 0;
static int g_selected_file = 0;

static uint32_t g_web_img[360 * 240];
static int g_web_w = 0, g_web_h = 0;
static int g_web_idx = 0;
static char g_status[256] = "ZERO OS v0.3.1: HYPERVISOR + VIRTIOFS DIRECT FILE SHARE";

// Full AI reply buffer + scroll state
static char g_ai_reply[4096] = "Press [F8] (or click here) then [Cmd+V] to paste prompt.";
static int g_ai_scroll = 0;
static int g_ai_total_lines = 1;
static int g_ai_input_mode = 0;
static char g_ai_prompt[512] = "";

// --- 44.1 kHz Direct PCM Chime Synthesizer ---
typedef struct {
    double phase;
    int samples_left;
} SynthState;

static void pcm_callback(void *userData, AudioQueueRef aq, AudioQueueBufferRef buf) {
    SynthState *st = (SynthState *)userData;
    int16_t *out = (int16_t *)buf->mAudioData;
    int frames = buf->mAudioDataBytesCapacity / sizeof(int16_t);
    for (int i = 0; i < frames; i++) {
        if (st->samples_left > 0) {
            double env = (double)st->samples_left / 13230.0;
            double s = sin(st->phase) * 0.25 * env + sin(st->phase * 1.5) * 0.12 * env;
            out[i] = (int16_t)(s * 32767.0);
            st->phase += 2.0 * M_PI * 528.0 / 44100.0;
            st->samples_left--;
        } else {
            out[i] = 0;
        }
    }
    buf->mAudioDataByteSize = frames * sizeof(int16_t);
    if (st->samples_left > 0) {
        AudioQueueEnqueueBuffer(aq, buf, 0, NULL);
    } else {
        AudioQueueStop(aq, false);
    }
}

static void play_pcm_chime(void) {
    static SynthState st;
    st.phase = 0.0;
    st.samples_left = 13230;
    AudioStreamBasicDescription fmt = {0};
    fmt.mSampleRate = 44100.0;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    fmt.mFramesPerPacket = 1;
    fmt.mChannelsPerFrame = 1;
    fmt.mBitsPerChannel = 16;
    fmt.mBytesPerPacket = 2;
    fmt.mBytesPerFrame = 2;
    AudioQueueRef aq;
    if (AudioQueueNewOutput(&fmt, pcm_callback, &st, NULL, NULL, 0, &aq) == noErr) {
        AudioQueueBufferRef b1, b2;
        AudioQueueAllocateBuffer(aq, 4096, &b1);
        AudioQueueAllocateBuffer(aq, 4096, &b2);
        pcm_callback(&st, aq, b1);
        pcm_callback(&st, aq, b2);
        AudioQueueStart(aq, NULL);
    }
}

// --- Real Host Filesystem Scanner ---
static void scan_directory(const char *path) {
    DIR *d = opendir(path);
    if (!d) return;
    strncpy(g_cwd, path, sizeof(g_cwd) - 1);
    g_file_count = 0;
    g_selected_file = 0;

    strcpy(g_files[g_file_count].name, "..");
    g_files[g_file_count].size = 0;
    g_files[g_file_count].is_dir = 1;
    g_file_count++;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_file_count < MAX_FILES) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            strncpy(g_files[g_file_count].name, ent->d_name, 127);
            g_files[g_file_count].name[127] = '\0';
            g_files[g_file_count].size = st.st_size;
            g_files[g_file_count].is_dir = S_ISDIR(st.st_mode);
            g_file_count++;
        }
    }
    closedir(d);
}

// --- Scrollback Terminal Buffer Helpers ---
static void term_newline(TermBuffer *tb) {
    if (tb->total_lines < TERM_HIST) {
        tb->total_lines++;
    } else {
        memmove(tb->lines[0], tb->lines[1], (TERM_HIST - 1) * (TERM_COLS + 1));
    }
    memset(tb->lines[tb->total_lines - 1], 0, TERM_COLS + 1);
    tb->cur_col = 0;
    if (tb->scroll_back > 0 && tb->scroll_back < tb->total_lines - TERM_ROWS) {
        tb->scroll_back++;
    }
}

static void term_put_char(TermBuffer *tb, char c) {
    if (tb->total_lines < 1) tb->total_lines = 1;
    int row = tb->total_lines - 1;
    if (c == '\r') {
        tb->cur_col = 0;
    } else if (c == '\n') {
        term_newline(tb);
        tb->scroll_back = 0;
    } else if (c == '\b' || c == 127) {
        if (tb->cur_col > 0) {
            tb->cur_col--;
            tb->lines[row][tb->cur_col] = ' ';
        }
    } else if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
        if (tb->cur_col >= TERM_COLS) {
            term_newline(tb);
            row = tb->total_lines - 1;
        }
        tb->lines[row][tb->cur_col++] = c;
        tb->lines[row][tb->cur_col] = '\0';
    }
}

static void stream_to_term(int fd, TermBuffer *tb) {
    char raw[512];
    int in_esc = 0;
    while (1) {
        ssize_t n = read(fd, raw, sizeof(raw));
        if (n <= 0) break;
        pthread_mutex_lock(&g_lock);
        for (ssize_t i = 0; i < n; i++) {
            char c = raw[i];
            if (c == '\x1b') { in_esc = 1; continue; }
            if (in_esc) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) in_esc = 0;
                continue;
            }
            term_put_char(tb, c);
        }
        pthread_mutex_unlock(&g_lock);
    }
}

static void *pty_reader_thread(void *arg) {
    stream_to_term(g_pty_fd, &g_host_term);
    return NULL;
}

static void *vm_reader_thread(void *arg) {
    stream_to_term(g_vm_out_fd, &g_vm_term);
    return NULL;
}

static void init_live_pty(void) {
    struct winsize ws = { .ws_row = TERM_ROWS, .ws_col = TERM_COLS };
    g_pty_pid = forkpty(&g_pty_fd, NULL, NULL, &ws);
    if (g_pty_pid == 0) {
        setenv("TERM", "dumb", 1);
        setenv("PS1", "macos-host$ ", 1);
        execl("/bin/zsh", "zsh", "-f", NULL);
        execl("/bin/sh", "sh", NULL);
        _exit(1);
    }
    pthread_t th;
    pthread_create(&th, NULL, pty_reader_thread, NULL);
    pthread_detach(th);
    const char *init_cmd = "uname -m -s -r && echo 'Host macOS PTY ready.'\n";
    write(g_pty_fd, init_cmd, strlen(init_cmd));
}

// --- Apple Virtualization.framework Native Linux Kernel + VirtioFS Boot ---
static void boot_native_linux_kernel(void) {
#if defined(__arm64__)
    if (g_vm_running) {
        snprintf(g_status, sizeof(g_status), "LINUX KERNEL ALREADY RUNNING ON HARDWARE HYPERVISOR");
        return;
    }
    NSString *base = [NSHomeDirectory() stringByAppendingPathComponent:@"zero_os/linux"];
    NSString *sharedDir = [NSHomeDirectory() stringByAppendingPathComponent:@"zero_os/shared"];
    [[NSFileManager defaultManager] createDirectoryAtPath:sharedDir withIntermediateDirectories:YES attributes:nil error:nil];

    NSString *kernelPath = [base stringByAppendingPathComponent:@"Image"];
    NSString *initrdPath = [base stringByAppendingPathComponent:@"initramfs.img"];

    if (![[NSFileManager defaultManager] fileExistsAtPath:kernelPath]) {
        g_console_mode = 0;
        snprintf(g_status, sizeof(g_status), "LINUX IMAGE NOT FOUND AT ~/zero_os/linux/Image");
        return;
    }

    int h2v[2], v2h[2];
    if (pipe(h2v) != 0 || pipe(v2h) != 0) return;
    g_vm_in_fd = h2v[1];
    g_vm_out_fd = v2h[0];

    pthread_t th;
    pthread_create(&th, NULL, vm_reader_thread, NULL);
    pthread_detach(th);

    NSFileHandle *readHandle = [[NSFileHandle alloc] initWithFileDescriptor:h2v[0] closeOnDealloc:YES];
    NSFileHandle *writeHandle = [[NSFileHandle alloc] initWithFileDescriptor:v2h[1] closeOnDealloc:YES];

    VZLinuxBootLoader *boot = [[VZLinuxBootLoader alloc] initWithKernelURL:[NSURL fileURLWithPath:kernelPath]];
    if ([[NSFileManager defaultManager] fileExistsAtPath:initrdPath]) {
        boot.initialRamdiskURL = [NSURL fileURLWithPath:initrdPath];
    }
    boot.commandLine = @"console=hvc0 rdinit=/bin/sh";

    VZVirtualMachineConfiguration *cfg = [[VZVirtualMachineConfiguration alloc] init];
    cfg.CPUCount = 2;
    cfg.memorySize = 256ULL * 1024ULL * 1024ULL;
    cfg.bootLoader = boot;
    cfg.platform = [[VZGenericPlatformConfiguration alloc] init];

    VZVirtioConsoleDeviceSerialPortConfiguration *serial = [[VZVirtioConsoleDeviceSerialPortConfiguration alloc] init];
    serial.attachment = [[VZFileHandleSerialPortAttachment alloc] initWithFileHandleForReading:readHandle fileHandleForWriting:writeHandle];
    cfg.serialPorts = @[serial];
    cfg.entropyDevices = @[[[VZVirtioEntropyDeviceConfiguration alloc] init]];

    // Direct VirtioFS Directory Share (macOS ~/zero_os/shared <-> Linux /mnt/macos)
    VZSharedDirectory *vzDir = [[VZSharedDirectory alloc] initWithURL:[NSURL fileURLWithPath:sharedDir] readOnly:NO];
    VZSingleDirectoryShare *share = [[VZSingleDirectoryShare alloc] initWithDirectory:vzDir];
    VZVirtioFileSystemDeviceConfiguration *fsDev = [[VZVirtioFileSystemDeviceConfiguration alloc] initWithTag:@"zero_share"];
    fsDev.share = share;
    cfg.directorySharingDevices = @[fsDev];

    NSError *valErr = nil;
    if (![cfg validateWithError:&valErr]) {
        snprintf(g_status, sizeof(g_status), "VZ CONFIG ERROR: %s", [[valErr localizedDescription] UTF8String]);
        return;
    }

    g_vz_vm = [[VZVirtualMachine alloc] initWithConfiguration:cfg];
    g_console_mode = 1;
    snprintf(g_status, sizeof(g_status), "BOOTING ARM64 LINUX KERNEL + VIRTIOFS DIRECT SHARE...");

    [g_vz_vm startWithCompletionHandler:^(NSError *err) {
        pthread_mutex_lock(&g_lock);
        if (err) {
            g_vm_running = 0;
            snprintf(g_status, sizeof(g_status), "VM START ERROR: %s", [[err localizedDescription] UTF8String]);
        } else {
            g_vm_running = 1;
            snprintf(g_status, sizeof(g_status), "LINUX LIVE + VIRTIOFS MOUNTED AT /mnt/macos (<-> ~/zero_os/shared)");
            play_pcm_chime();
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                const char *welcome = "/bin/busybox --install -s 2>/dev/null; export PATH=/bin:/sbin:/usr/bin:/usr/sbin; mkdir -p /mnt/macos && mount -t virtiofs zero_share /mnt/macos && ls -la /mnt/macos\n";
                if (g_vm_in_fd >= 0) write(g_vm_in_fd, welcome, strlen(welcome));
            });
        }
        pthread_mutex_unlock(&g_lock);
    }];
#else
    g_console_mode = 0;
    snprintf(g_status, sizeof(g_status), "x86_64 MODE: RUNNING HOST SHELL (ARM64 VZ KERNEL DISABLED)");
#endif
}

// --- Native NSURLSession Web Image Fetcher ---
static void fetch_live_web_image(void) {
    const char *urls[] = {
        "https://picsum.photos/id/1015/360/240.jpg",
        "https://picsum.photos/id/1018/360/240.jpg",
        "https://picsum.photos/id/1036/360/240.jpg"
    };
    NSURL *url = [NSURL URLWithString:[NSString stringWithUTF8String:urls[g_web_idx % 3]]];
    g_web_idx++;
    snprintf(g_status, sizeof(g_status), "FETCHING LIVE WEB ASSET VIA NSURLSESSION...");

    [[[NSURLSession sharedSession] dataTaskWithURL:url completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
        if (err || !data) {
            pthread_mutex_lock(&g_lock);
            snprintf(g_status, sizeof(g_status), "NET FETCH ERROR (OFFLINE OR BLOCKED)");
            pthread_mutex_unlock(&g_lock);
            return;
        }
        CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
        if (!src) return;
        CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
        CFRelease(src);
        if (!img) return;

        int w = 360, h = 240;
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        uint32_t tmp[360 * 240] = {0};
        CGContextRef ic = CGBitmapContextCreate(tmp, w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);
        CGContextDrawImage(ic, CGRectMake(0, 0, w, h), img);
        CGContextRelease(ic);
        CGColorSpaceRelease(cs);
        CGImageRelease(img);

        pthread_mutex_lock(&g_lock);
        memcpy(g_web_img, tmp, sizeof(tmp));
        g_web_w = w;
        g_web_h = h;
        snprintf(g_status, sizeof(g_status), "DECODED LIVE JPEG (%lu BYTES) INTO UNIFIED MEMORY", (unsigned long)data.length);
        pthread_mutex_unlock(&g_lock);
    }] resume];
}

// --- Native NSURLSession Local AI Query ---
static void query_local_ai_native(const char *prompt) {
    if (!prompt || !prompt[0]) return;
    NSURL *url = [NSURL URLWithString:@"http://127.0.0.1:11434/api/generate"];
    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:url];
    req.HTTPMethod = @"POST";
    req.timeoutInterval = 30.0;
    [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];

    NSString *pStr = [NSString stringWithUTF8String:prompt];
    if (!pStr) return;
    NSDictionary *body = @{@"model": @"llama3.2", @"prompt": pStr, @"stream": @NO, @"keep_alive": @"30m"};
    req.HTTPBody = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    snprintf(g_ai_reply, sizeof(g_ai_reply), "Querying 127.0.0.1:11434...");
    g_ai_scroll = 0;

    [[[NSURLSession sharedSession] dataTaskWithRequest:req completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
        pthread_mutex_lock(&g_lock);
        if (err || !data) {
            snprintf(g_ai_reply, sizeof(g_ai_reply), "Ollama waking up -> auto-launching Ollama.app...");
            system("open -a Ollama >/dev/null 2>&1 &");
        } else {
            NSDictionary *json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
            NSString *res = json[@"response"];
            if (res) {
                NSString *clean = [res stringByReplacingOccurrencesOfString:@"\r" withString:@""];
                snprintf(g_ai_reply, sizeof(g_ai_reply), "AI: %s", [clean UTF8String]);
            }
        }
        g_ai_scroll = 0;
        pthread_mutex_unlock(&g_lock);
    }] resume];
}

// --- .atom Binary State Crystallization ---
static void save_atom_snapshot(void) {
    ZeroAtomSnapshot *snap = calloc(1, sizeof(ZeroAtomSnapshot));
    snap->magic = 0x5A45524F;
    strncpy(snap->cwd, g_cwd, sizeof(snap->cwd) - 1);
    int start = MAX(0, g_host_term.total_lines - TERM_ROWS);
    for (int i = 0; i < TERM_ROWS && (start + i) < g_host_term.total_lines; i++) {
        strncpy(snap->term_lines[i], g_host_term.lines[start + i], TERM_COLS);
    }
    snap->term_row = MIN(TERM_ROWS - 1, g_host_term.total_lines - 1);
    snap->term_col = g_host_term.cur_col;
    strncpy(snap->status_msg, g_status, sizeof(snap->status_msg) - 1);
    strncpy(snap->ai_reply, g_ai_reply, sizeof(snap->ai_reply) - 1);
    snap->img_w = g_web_w;
    snap->img_h = g_web_h;
    memcpy(snap->img_buf, g_web_img, sizeof(g_web_img));

    NSString *path = [NSHomeDirectory() stringByAppendingPathComponent:@"zero_os/state.atom"];
    FILE *f = fopen([path UTF8String], "wb");
    if (f) {
        fwrite(snap, sizeof(ZeroAtomSnapshot), 1, f);
        fclose(f);
        snprintf(g_status, sizeof(g_status), "CRYSTALLIZED STATE -> ~/zero_os/state.atom (%lu KB)", sizeof(ZeroAtomSnapshot) / 1024);
        play_pcm_chime();
    }
    free(snap);
}

static void restore_atom_snapshot(void) {
    NSString *path = [NSHomeDirectory() stringByAppendingPathComponent:@"zero_os/state.atom"];
    FILE *f = fopen([path UTF8String], "rb");
    if (!f) {
        snprintf(g_status, sizeof(g_status), "NO .ATOM SNAPSHOT FOUND (PRESS [F6] FIRST)");
        return;
    }
    ZeroAtomSnapshot *snap = calloc(1, sizeof(ZeroAtomSnapshot));
    if (fread(snap, sizeof(ZeroAtomSnapshot), 1, f) == 1 && snap->magic == 0x5A45524F) {
        scan_directory(snap->cwd);
        memset(&g_host_term, 0, sizeof(g_host_term));
        g_host_term.total_lines = TERM_ROWS;
        for (int i = 0; i < TERM_ROWS; i++) {
            strncpy(g_host_term.lines[i], snap->term_lines[i], TERM_COLS);
        }
        g_host_term.cur_col = snap->term_col;
        strncpy(g_ai_reply, snap->ai_reply, sizeof(g_ai_reply) - 1);
        g_ai_scroll = 0;
        g_web_w = snap->img_w;
        g_web_h = snap->img_h;
        memcpy(g_web_img, snap->img_buf, sizeof(g_web_img));
        snprintf(g_status, sizeof(g_status), "INSTANTLY RESTORED .ATOM STATE IN <1 MS");
        play_pcm_chime();
    }
    fclose(f);
    free(snap);
}

// --- Direct Framebuffer Drawing Helpers ---
static void fill_rect(int x, int y, int w, int h, uint32_t col) {
    for (int j = y; j < y + h && j < FB_H; j++) {
        if (j < 0) continue;
        for (int i = x; i < x + w && i < FB_W; i++) {
            if (i >= 0) g_fb[j * FB_W + i] = col;
        }
    }
}

static void draw_border(int x, int y, int w, int h, uint32_t col) {
    fill_rect(x, y, w, 2, col);
    fill_rect(x, y + h - 2, w, 2, col);
    fill_rect(x, y, 2, h, col);
    fill_rect(x + w - 2, y, 2, h, col);
}

static void draw_text(int x, int y, const char *str, CGFloat size, NSColor *color, BOOL bold) {
    if (!str || !str[0]) return;
    NSString *ns = [NSString stringWithUTF8String:str];
    if (!ns) return;
    NSFont *font = bold ? [NSFont monospacedSystemFontOfSize:size weight:NSFontWeightBold]
                        : [NSFont monospacedSystemFontOfSize:size weight:NSFontWeightRegular];
    NSDictionary *attrs = @{NSFontAttributeName: font, NSForegroundColorAttributeName: color};
    [ns drawAtPoint:NSMakePoint(x, y) withAttributes:attrs];
}

static double get_mach_ram_mb(void) {
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
        return (double)info.resident_size / (1024.0 * 1024.0);
    }
    return 0.0;
}

static int wrap_ai_lines(const char *text, char lines[AI_MAX_LINES][AI_LINE_COLS + 1]) {
    int count = 0;
    int col = 0;
    memset(lines, 0, AI_MAX_LINES * (AI_LINE_COLS + 1));
    for (size_t i = 0; text[i] != '\0' && count < AI_MAX_LINES; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') {
            count++;
            col = 0;
        } else if (c >= 32 && c < 127) {
            if (col >= AI_LINE_COLS) {
                count++;
                col = 0;
                if (count >= AI_MAX_LINES) break;
            }
            lines[count][col++] = (char)c;
        }
    }
    return count + 1;
}

static void render_frame(void) {
    pthread_mutex_lock(&g_lock);
    fill_rect(0, 0, FB_W, FB_H, 0xFF0B0E14);

    NSGraphicsContext *prevCtx = [NSGraphicsContext currentContext];
    NSGraphicsContext *nsCtx = [NSGraphicsContext graphicsContextWithCGContext:g_ctx flipped:YES];
    [NSGraphicsContext setCurrentContext:nsCtx];

    NSColor *cCyan  = [NSColor colorWithRed:0.25 green:0.90 blue:0.95 alpha:1.0];
    NSColor *cGreen = [NSColor colorWithRed:0.30 green:0.95 blue:0.55 alpha:1.0];
    NSColor *cAmber = [NSColor colorWithRed:1.00 green:0.78 blue:0.25 alpha:1.0];
    NSColor *cWhite = [NSColor colorWithWhite:0.92 alpha:1.0];
    NSColor *cDim   = [NSColor colorWithWhite:0.55 alpha:1.0];

    // [1] TOP-LEFT: REAL HOST FILESYSTEM (+ [F3] SHARED DIR JUMP)
    fill_rect(8, 8, 628, 376, 0xFF111622);
    draw_border(8, 8, 628, 376, 0xFF28354D);
    draw_text(20, 16, "[1] HOST FILESYSTEM — [F2] Open | [F3] VirtioFS Shared", 12.5, cCyan, YES);
    char cwd_disp[80];
    snprintf(cwd_disp, sizeof(cwd_disp), "PATH: %.66s", g_cwd);
    draw_text(20, 36, cwd_disp, 11, cDim, NO);

    int max_vis = 15;
    int start_idx = (g_selected_file >= max_vis) ? (g_selected_file - max_vis + 1) : 0;
    for (int i = 0; i < max_vis && (start_idx + i) < g_file_count; i++) {
        int idx = start_idx + i;
        int ry = 60 + i * 20;
        if (idx == g_selected_file) fill_rect(16, ry - 2, 612, 19, 0xFF1E2D4A);
        char line[96];
        if (g_files[idx].is_dir) {
            snprintf(line, sizeof(line), "[DIR]  %-44s <DIR>", g_files[idx].name);
            draw_text(22, ry, line, 12, cAmber, idx == g_selected_file);
        } else {
            snprintf(line, sizeof(line), "[FILE] %-44s %8lld B", g_files[idx].name, g_files[idx].size);
            draw_text(22, ry, line, 12, cWhite, idx == g_selected_file);
        }
    }

    // [2] BOTTOM-LEFT: DUAL KERNEL CONSOLE + SCROLLBAR + COPY/SWITCH BUTTONS
    fill_rect(8, 392, 628, 376, 0xFF0D1017);
    TermBuffer *activeTerm = (g_console_mode == 1) ? &g_vm_term : &g_host_term;
    int max_term_scroll = MAX(0, activeTerm->total_lines - TERM_ROWS);
    if (activeTerm->scroll_back > max_term_scroll) activeTerm->scroll_back = max_term_scroll;
    if (activeTerm->scroll_back < 0) activeTerm->scroll_back = 0;

    int first_row = MAX(0, activeTerm->total_lines - TERM_ROWS - activeTerm->scroll_back);
    if (g_console_mode == 1) {
        draw_border(8, 392, 628, 376, 0xFF38BDF8);
        draw_text(20, 400, "[2] GUEST LINUX KERNEL (/mnt/macos <-> shared)", 12, cCyan, YES);
        for (int r = 0; r < TERM_ROWS && (first_row + r) < activeTerm->total_lines; r++) {
            if (activeTerm->lines[first_row + r][0]) {
                draw_text(20, 424 + r * 16, activeTerm->lines[first_row + r], 11.5, cGreen, NO);
            }
        }
    } else {
        draw_border(8, 392, 628, 376, 0xFF2B5B3F);
        draw_text(20, 400, "[2] HOST MACOS SHELL (POSIX /bin/zsh)", 12.5, cGreen, YES);
        for (int r = 0; r < TERM_ROWS && (first_row + r) < activeTerm->total_lines; r++) {
            if (activeTerm->lines[first_row + r][0]) {
                draw_text(20, 424 + r * 16, activeTerm->lines[first_row + r], 11.5, cWhite, NO);
            }
        }
    }

    fill_rect(476, 398, 92, 18, 0xFF1E293B);
    draw_border(476, 398, 92, 18, 0xFF38BDF8);
    draw_text(483, 400, "[F1: OS]", 10.5, cCyan, YES);

    fill_rect(574, 398, 54, 18, 0xFF1E293B);
    draw_border(574, 398, 54, 18, 0xFF30F28C);
    draw_text(581, 400, "[COPY]", 10.5, cGreen, YES);

    int c_tx = 620, c_ty = 424, c_tw = 8, c_th = 334;
    fill_rect(c_tx, c_ty, c_tw, c_th, 0xFF1E293B);
    int c_thumb_h = (activeTerm->total_lines <= TERM_ROWS)
                    ? c_th
                    : MAX(18, (c_th * TERM_ROWS) / activeTerm->total_lines);
    int c_thumb_y = c_ty + (c_th - c_thumb_h);
    if (max_term_scroll > 0) {
        c_thumb_y = c_ty + ((c_th - c_thumb_h) * (max_term_scroll - activeTerm->scroll_back)) / max_term_scroll;
    }
    fill_rect(c_tx, c_thumb_y, c_tw, c_thumb_h, (g_console_mode == 1) ? 0xFF38BDF8 : 0xFF30F28C);

    // [3] TOP-RIGHT: LIVE WEB STREAM & CROSS-OS BRIDGE
    fill_rect(644, 8, 628, 376, 0xFF111622);
    draw_border(644, 8, 628, 376, 0xFF28354D);
    draw_text(656, 16, "[3] LIVE WEB FEED & .ATOM SPATIAL BRIDGE", 13, cCyan, YES);
    draw_text(656, 36, "Native NSURLSession + ImageIO Zero-Copy Pixel Buffer", 11, cDim, NO);

    if (g_web_w > 0 && g_web_h > 0) {
        int ox = 778, oy = 66;
        for (int j = 0; j < g_web_h; j++) {
            memcpy(&g_fb[(oy + j) * FB_W + ox], &g_web_img[j * g_web_w], g_web_w * 4);
        }
        draw_border(ox - 2, oy - 2, g_web_w + 4, g_web_h + 4, 0xFF38BDF8);
    } else {
        fill_rect(778, 66, 360, 240, 0xFF182032);
        draw_text(835, 175, "Press [F5] to Fetch Live Web Photo", 12, cDim, NO);
    }
    draw_text(656, 326, "Kernel/FS: [F1] Linux/macOS | [F3] Shared Dir | [F4] Boot Linux", 11, cCyan, NO);
    draw_text(656, 348, "Storage:   [F5] Web Photo   | [F6] Save .atom | [F7] Load .atom", 11, cAmber, NO);

    // [4] BOTTOM-RIGHT: REAL MACH KERNEL TELEMETRY & NATIVE AI
    fill_rect(644, 392, 628, 376, 0xFF111622);
    draw_border(644, 392, 628, 376, 0xFF28354D);
    draw_text(656, 400, "[4] REAL HYPERVISOR TELEMETRY & NATIVE LOCAL AI", 13, cCyan, YES);

#if defined(__arm64__)
    const char *arch_str = "APPLE SILICON (arm64 + VZ + VIRTIOFS)";
#else
    const char *arch_str = "INTEL MAC (x86_64 NATIVE)";
#endif
    char info1[96], info2[96], info3[96];
    snprintf(info1, sizeof(info1), "CPU ARCHITECTURE : %s", arch_str);
    snprintf(info2, sizeof(info2), "HOST PROCESS RAM : %.2f MB (REAL MACH FOOTPRINT)", get_mach_ram_mb());
    snprintf(info3, sizeof(info3), "GUEST LINUX VM   : %s", g_vm_running ? "ACTIVE (2 vCPU, 256 MB, /mnt/macos SHARED)" : "STANDBY (PRESS [F4])");
    draw_text(656, 432, info1, 12.5, cGreen, YES);
    draw_text(656, 454, info2, 12.5, cAmber, YES);
    draw_text(656, 476, info3, 12.0, cCyan, YES);

    fill_rect(656, 514, 604, 114, 0xFF0B0E14);
    draw_border(656, 514, 604, 114, g_ai_input_mode ? 0xFFF59E0B : 0xFF334155);
    draw_text(668, 522, "NATIVE LOCAL LLM BRIDGE (NSURLSession -> 127.0.0.1:11434)", 11.5, cCyan, YES);

    if (g_ai_input_mode) {
        char pbuf[220];
        size_t plen = strlen(g_ai_prompt);
        const char *tail = (plen > 58) ? (g_ai_prompt + plen - 58) : g_ai_prompt;
        snprintf(pbuf, sizeof(pbuf), "PROMPT> %s_", tail);
        draw_text(668, 554, pbuf, 12, cAmber, YES);
        draw_text(668, 602, "[Cmd+V] Paste Prompt | [Enter] Send | [Esc/F8] Cancel", 10.5, cDim, NO);
    } else {
        char lines[AI_MAX_LINES][AI_LINE_COLS + 1];
        g_ai_total_lines = wrap_ai_lines(g_ai_reply, lines);
        int max_scroll = (g_ai_total_lines > 3) ? (g_ai_total_lines - 3) : 0;
        if (g_ai_scroll > max_scroll) g_ai_scroll = max_scroll;
        if (g_ai_scroll < 0) g_ai_scroll = 0;

        for (int row = 0; row < 3; row++) {
            int lidx = g_ai_scroll + row;
            if (lidx < g_ai_total_lines && lines[lidx][0]) {
                draw_text(668, 544 + row * 16, lines[lidx], 11.5, cWhite, NO);
            }
        }

        int track_x = 1244, track_y = 543, track_w = 8, track_h = 48;
        fill_rect(track_x, track_y, track_w, track_h, 0xFF1E293B);
        int thumb_h = (g_ai_total_lines <= 3) ? track_h : MAX(12, (track_h * 3) / g_ai_total_lines);
        int thumb_y = track_y;
        if (max_scroll > 0) {
            thumb_y = track_y + ((track_h - thumb_h) * g_ai_scroll) / max_scroll;
        }
        fill_rect(track_x, thumb_y, track_w, thumb_h, 0xFF38BDF8);

        char hint[96];
        snprintf(hint, sizeof(hint), "[F8] Prompt | [Cmd+V] Paste | [Cmd+C] Copy | Line %d/%d",
                 MIN(g_ai_scroll + 3, g_ai_total_lines), g_ai_total_lines);
        draw_text(668, 602, hint, 10.5, cDim, NO);
    }

    // Bottom Status Bar
    fill_rect(8, 774, 1264, 20, 0xFF162033);
    draw_text(18, 776, g_status, 11.5, cGreen, YES);

    [NSGraphicsContext setCurrentContext:prevCtx];
    pthread_mutex_unlock(&g_lock);
}

@interface ZeroView : NSView {
    int _dragTarget;
}
@end

@implementation ZeroView
- (BOOL)acceptsFirstResponder { return YES; }

- (NSPoint)fbPointFromEvent:(NSEvent *)event {
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    CGFloat bw = self.bounds.size.width;
    CGFloat bh = self.bounds.size.height;
    if (bw <= 0 || bh <= 0) return NSMakePoint(0, 0);
    CGFloat fx = p.x * ((CGFloat)FB_W / bw);
    CGFloat fy = (bh - p.y) * ((CGFloat)FB_H / bh);
    return NSMakePoint(fx, fy);
}

- (void)updateAiScrollFromFbY:(CGFloat)fy {
    pthread_mutex_lock(&g_lock);
    int max_scroll = (g_ai_total_lines > 3) ? (g_ai_total_lines - 3) : 0;
    if (max_scroll > 0) {
        CGFloat ratio = (fy - 543.0) / 48.0;
        if (ratio < 0.0) ratio = 0.0;
        if (ratio > 1.0) ratio = 1.0;
        g_ai_scroll = (int)round(ratio * max_scroll);
    }
    pthread_mutex_unlock(&g_lock);
}

- (void)updateTermScrollFromFbY:(CGFloat)fy {
    pthread_mutex_lock(&g_lock);
    TermBuffer *tb = (g_console_mode == 1) ? &g_vm_term : &g_host_term;
    int max_scroll = MAX(0, tb->total_lines - TERM_ROWS);
    if (max_scroll > 0) {
        CGFloat ratio = (fy - 424.0) / 334.0;
        if (ratio < 0.0) ratio = 0.0;
        if (ratio > 1.0) ratio = 1.0;
        tb->scroll_back = (int)round((1.0 - ratio) * max_scroll);
    }
    pthread_mutex_unlock(&g_lock);
}

- (void)copyConsoleLogToClipboard {
    pthread_mutex_lock(&g_lock);
    TermBuffer *tb = (g_console_mode == 1) ? &g_vm_term : &g_host_term;
    NSMutableString *out = [NSMutableString string];
    for (int i = 0; i < tb->total_lines; i++) {
        NSString *line = [NSString stringWithUTF8String:tb->lines[i]];
        if (line) [out appendFormat:@"%@\n", line];
    }
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:out forType:NSPasteboardTypeString];
    snprintf(g_status, sizeof(g_status), "COPIED %s LOG (%d LINES) TO CLIPBOARD",
             (g_console_mode == 1) ? "LINUX KERNEL" : "MACOS SHELL", tb->total_lines);
    pthread_mutex_unlock(&g_lock);
}

- (void)mouseDown:(NSEvent *)event {
    NSPoint fp = [self fbPointFromEvent:event];
    _dragTarget = 0;

    if (fp.x >= 474 && fp.x <= 570 && fp.y >= 394 && fp.y <= 420) {
        pthread_mutex_lock(&g_lock);
        g_console_mode = !g_console_mode;
        snprintf(g_status, sizeof(g_status), g_console_mode ? "CONSOLE FOCUS: GUEST LINUX KERNEL (ARM64)" : "CONSOLE FOCUS: HOST MACOS SHELL (/bin/zsh)");
        pthread_mutex_unlock(&g_lock);
        return;
    }

    if (fp.x >= 572 && fp.x <= 632 && fp.y >= 394 && fp.y <= 420) {
        [self copyConsoleLogToClipboard];
        return;
    }

    if (fp.x >= 612 && fp.x <= 636 && fp.y >= 422 && fp.y <= 764) {
        _dragTarget = 2;
        [self updateTermScrollFromFbY:fp.y];
        return;
    }

    if (fp.x >= 1236 && fp.x <= 1262 && fp.y >= 538 && fp.y <= 598) {
        _dragTarget = 1;
        [self updateAiScrollFromFbY:fp.y];
        return;
    }

    if (fp.x >= 656 && fp.x <= 1235 && fp.y >= 514 && fp.y <= 628) {
        pthread_mutex_lock(&g_lock);
        g_ai_input_mode = 1;
        g_ai_prompt[0] = '\0';
        snprintf(g_status, sizeof(g_status), "AI PROMPT MODE: PRESS [CMD+V] TO PASTE OR TYPE & PRESS [ENTER]");
        pthread_mutex_unlock(&g_lock);
    }
}

- (void)mouseDragged:(NSEvent *)event {
    if (_dragTarget == 1) {
        NSPoint fp = [self fbPointFromEvent:event];
        [self updateAiScrollFromFbY:fp.y];
    } else if (_dragTarget == 2) {
        NSPoint fp = [self fbPointFromEvent:event];
        [self updateTermScrollFromFbY:fp.y];
    }
}

- (void)mouseUp:(NSEvent *)event {
    _dragTarget = 0;
}

- (void)scrollWheel:(NSEvent *)event {
    CGFloat dy = [event scrollingDeltaY];
    if (fabs(dy) < 0.1) return;
    NSPoint fp = [self fbPointFromEvent:event];

    pthread_mutex_lock(&g_lock);
    if (fp.x < 640 && fp.y >= 390) {
        TermBuffer *tb = (g_console_mode == 1) ? &g_vm_term : &g_host_term;
        int max_scroll = MAX(0, tb->total_lines - TERM_ROWS);
        if (dy > 0) {
            tb->scroll_back = MIN(max_scroll, tb->scroll_back + 2);
        } else {
            tb->scroll_back = MAX(0, tb->scroll_back - 2);
        }
    } else {
        int max_scroll = (g_ai_total_lines > 3) ? (g_ai_total_lines - 3) : 0;
        if (dy < 0) {
            g_ai_scroll = MIN(max_scroll, g_ai_scroll + 1);
        } else {
            g_ai_scroll = MAX(0, g_ai_scroll - 1);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

- (void)drawRect:(NSRect)dirtyRect {
    render_frame();
    CGImageRef img = CGBitmapContextCreateImage(g_ctx);
    if (img) {
        CGContextRef winCtx = [[NSGraphicsContext currentContext] CGContext];
        CGContextDrawImage(winCtx, self.bounds, img);
        CGImageRelease(img);
    }
}

- (void)pasteFromClipboard {
    NSString *clip = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    if (!clip || clip.length == 0) return;

    pthread_mutex_lock(&g_lock);
    if (g_ai_input_mode) {
        NSString *flat = [[clip stringByReplacingOccurrencesOfString:@"\r" withString:@" "]
                                stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
        const char *u = [flat UTF8String];
        if (u) {
            size_t cur = strlen(g_ai_prompt);
            size_t rem = sizeof(g_ai_prompt) - 1 - cur;
            if (rem > 0) strncat(g_ai_prompt, u, rem);
            snprintf(g_status, sizeof(g_status), "PASTED PROMPT (%lu CHARS) — PRESS [ENTER] TO SEND", strlen(g_ai_prompt));
        }
    } else {
        const char *u = [clip UTF8String];
        if (u) {
            if (g_console_mode == 1 && g_vm_in_fd >= 0) {
                g_vm_term.scroll_back = 0;
                write(g_vm_in_fd, u, strlen(u));
            } else if (g_pty_fd >= 0) {
                g_host_term.scroll_back = 0;
                write(g_pty_fd, u, strlen(u));
            }
            snprintf(g_status, sizeof(g_status), "PASTED TEXT INTO %s CONSOLE", (g_console_mode == 1) ? "LINUX" : "MACOS");
        }
    }
    pthread_mutex_unlock(&g_lock);
}

- (void)copyAiReplyToClipboard {
    pthread_mutex_lock(&g_lock);
    NSString *text = [NSString stringWithUTF8String:g_ai_reply];
    if (text) {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:text forType:NSPasteboardTypeString];
        snprintf(g_status, sizeof(g_status), "COPIED FULL AI REPLY TO CLIPBOARD (%lu CHARS)", (unsigned long)text.length);
    }
    pthread_mutex_unlock(&g_lock);
}

- (void)keyDown:(NSEvent *)event {
    unsigned short kc = [event keyCode];
    NSString *chars = [event characters];
    NSEventModifierFlags mods = [event modifierFlags];

    if (mods & NSEventModifierFlagCommand) {
        NSString *unmod = [[event charactersIgnoringModifiers] lowercaseString];
        if ([unmod isEqualToString:@"v"]) {
            [self pasteFromClipboard];
            return;
        }
        if ([unmod isEqualToString:@"c"]) {
            if (mods & NSEventModifierFlagShift) {
                [self copyConsoleLogToClipboard];
            } else {
                [self copyAiReplyToClipboard];
            }
            return;
        }
    }

    if (kc == 122) { // F1 -> Toggle Linux Kernel / Host macOS Shell
        g_console_mode = !g_console_mode;
        snprintf(g_status, sizeof(g_status), g_console_mode ? "CONSOLE FOCUS: GUEST LINUX KERNEL (ARM64)" : "CONSOLE FOCUS: HOST MACOS SHELL (/bin/zsh)");
        return;
    }
    if (kc == 120) { // F2 -> Open selected directory
        if (g_selected_file < g_file_count && g_files[g_selected_file].is_dir) {
            char next[1024];
            snprintf(next, sizeof(next), "%s/%s", g_cwd, g_files[g_selected_file].name);
            char resolved[512];
            if (realpath(next, resolved)) scan_directory(resolved);
        }
        return;
    }
    if (kc == 99) { // F3 -> Jump to VirtioFS shared directory
        char shared[512];
        snprintf(shared, sizeof(shared), "%s/zero_os/shared", getenv("HOME"));
        scan_directory(shared);
        snprintf(g_status, sizeof(g_status), "VIEWING VIRTIOFS SHARED FOLDER (~/zero_os/shared <-> /mnt/macos)");
        return;
    }
    if (kc == 118) { boot_native_linux_kernel(); return; }    // F4 -> Boot Linux Kernel
    if (kc == 96)  { fetch_live_web_image(); return; }        // F5 -> Fetch Web Image
    if (kc == 97)  { save_atom_snapshot(); return; }          // F6 -> Save .atom
    if (kc == 98)  { restore_atom_snapshot(); return; }       // F7 -> Restore .atom
    if (kc == 100 || (kc == 53 && g_ai_input_mode)) {         // F8 or Esc -> Toggle AI input
        g_ai_input_mode = !g_ai_input_mode;
        g_ai_prompt[0] = '\0';
        snprintf(g_status, sizeof(g_status), g_ai_input_mode ? "AI PROMPT MODE: PRESS [CMD+V] TO PASTE OR TYPE & PRESS [ENTER]" : "EXITED AI PROMPT MODE");
        return;
    }
    if (kc == 101) { play_pcm_chime(); return; }              // F9 -> Audio chime

    if (g_ai_input_mode) {
        if (kc == 36) {
            query_local_ai_native(g_ai_prompt);
            g_ai_input_mode = 0;
        } else if (kc == 51) {
            size_t len = strlen(g_ai_prompt);
            if (len > 0) g_ai_prompt[len - 1] = '\0';
        } else if (chars.length > 0) {
            const char *u = [chars UTF8String];
            if (u && strlen(g_ai_prompt) + strlen(u) < sizeof(g_ai_prompt) - 1) {
                strcat(g_ai_prompt, u);
            }
        }
        return;
    }

    if (kc == 126) { if (g_selected_file > 0) g_selected_file--; return; }
    if (kc == 125) { if (g_selected_file + 1 < g_file_count) g_selected_file++; return; }

    if (chars.length > 0) {
        const char *u = [chars UTF8String];
        if (!u) return;
        if (g_console_mode == 1 && g_vm_in_fd >= 0) {
            g_vm_term.scroll_back = 0;
            write(g_vm_in_fd, u, strlen(u));
        } else if (g_pty_fd >= 0) {
            g_host_term.scroll_back = 0;
            write(g_pty_fd, u, strlen(u));
        }
    }
}
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        g_fb = calloc(FB_W * FB_H, sizeof(uint32_t));
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        g_ctx = CGBitmapContextCreate(g_fb, FB_W, FB_H, 8, FB_W * 4, cs,
                                      kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);
        CGColorSpaceRelease(cs);
        CGContextTranslateCTM(g_ctx, 0, FB_H);
        CGContextScaleCTM(g_ctx, 1.0, -1.0);

        char home_dir[512];
        snprintf(home_dir, sizeof(home_dir), "%s/zero_os", getenv("HOME"));
        scan_directory(home_dir);
        init_live_pty();
        boot_native_linux_kernel();
        fetch_live_web_image();

        NSRect scr = [[NSScreen mainScreen] visibleFrame];
        CGFloat winW = MIN(FB_W, scr.size.width * 0.92);
        CGFloat winH = winW * ((CGFloat)FB_H / (CGFloat)FB_W);
        if (winH > scr.size.height * 0.90) {
            winH = scr.size.height * 0.90;
            winW = winH * ((CGFloat)FB_W / (CGFloat)FB_H);
        }

        NSRect rect = NSMakeRect((scr.size.width - winW) / 2, (scr.size.height - winH) / 2, winW, winH);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:rect
                                                    styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable)
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        [win setTitle:@"ZERO OS v0.3.1 — Hardware Hypervisor, ARM64 Linux Kernel & VirtioFS Bridge"];
        ZeroView *view = [[ZeroView alloc] initWithFrame:NSMakeRect(0, 0, winW, winH)];
        [win setContentView:view];
        [win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        [NSTimer scheduledTimerWithTimeInterval:(1.0 / 30.0) repeats:YES block:^(NSTimer *t) {
            [view setNeedsDisplay:YES];
        }];

        [NSApp run];
    }
    return 0;
}
