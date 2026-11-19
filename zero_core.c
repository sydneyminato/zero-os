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
#define WIN_COUNT 4
#define TERM_HIST 240
#define TERM_MAX_COLS 120
#define MAX_FILES 64
#define AI_MAX_LINES 120
#define AI_MAX_COLS 120

enum {
    WIN_LINUX = 0,
    WIN_MACOS = 1,
    WIN_FILES = 2,
    WIN_AI_HUD = 3
};

typedef struct {
    int id;
    char title[96];
    int x, y, w, h;
    int min_w, min_h;
    uint32_t border_col;
    uint32_t title_bg;
    uint32_t body_bg;
} SpatialWindow;

typedef struct {
    char name[128];
    off_t size;
    int is_dir;
} FileEntry;

typedef struct {
    char lines[TERM_HIST][TERM_MAX_COLS + 1];
    int total_lines;
    int cur_col;
    int scroll_back;
} TermBuffer;

typedef struct {
    uint32_t magic;
    SpatialWindow wins[WIN_COUNT];
    int z_order[WIN_COUNT];
    char cwd[512];
    char status_msg[256];
    char ai_reply[4096];
    int img_w;
    int img_h;
    uint32_t img_buf[360 * 240];
} ZeroAtomSnapshot;

static uint32_t *g_fb = NULL;
static CGContextRef g_ctx = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static SpatialWindow g_wins[WIN_COUNT];
static int g_z_order[WIN_COUNT] = { WIN_FILES, WIN_AI_HUD, WIN_MACOS, WIN_LINUX }; // last = frontmost

// Host PTY shell state
static int g_pty_fd = -1;
static pid_t g_pty_pid = -1;
static TermBuffer g_host_term = { .total_lines = 1 };

// Guest Linux Kernel VM state
static int g_vm_in_fd = -1;
static int g_vm_out_fd = -1;
static TermBuffer g_vm_term = { .total_lines = 1 };
static int g_vm_running = 0;
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
static char g_status[256] = "ZERO OS v0.4.0: SPATIAL MULTI-KERNEL WINDOW COMPOSITOR ONLINE";

static char g_ai_reply[4096] = "Click AI box or press [F8], then [Cmd+V] to paste prompt.";
static int g_ai_scroll = 0;
static int g_ai_total_lines = 1;
static int g_ai_input_mode = 0;
static char g_ai_prompt[512] = "";

// --- Spatial Window Layout Initialization ---
static void reset_spatial_layout(void) {
    // 1. Guest Linux Kernel: Wide horizontal console (top-left)
    g_wins[WIN_LINUX] = (SpatialWindow){
        .id = WIN_LINUX,
        .title = "GUEST LINUX KERNEL (ARM64 HYPERVISOR | /mnt/macos)",
        .x = 24, .y = 24, .w = 660, .h = 400,
        .min_w = 360, .min_h = 200,
        .border_col = 0xFF38BDF8,
        .title_bg   = 0xFF102A43,
        .body_bg    = 0xFF0B111A
    };

    // 2. Host macOS Shell: Overlapping independent terminal (bottom-right-center)
    g_wins[WIN_MACOS] = (SpatialWindow){
        .id = WIN_MACOS,
        .title = "HOST MACOS SHELL (POSIX /bin/zsh)",
        .x = 590, .y = 360, .w = 656, .h = 392,
        .min_w = 340, .min_h = 190,
        .border_col = 0xFF30F28C,
        .title_bg   = 0xFF123524,
        .body_bg    = 0xFF0C1410
    };

    // 3. Host & VirtioFS Filesystem: Tall vertical browser (top-right)
    g_wins[WIN_FILES] = (SpatialWindow){
        .id = WIN_FILES,
        .title = "FILESYSTEM & VIRTIOFS SHARED ([F2] Open | [F3] Shared)",
        .x = 716, .y = 24, .w = 536, .h = 316,
        .min_w = 320, .min_h = 180,
        .border_col = 0xFFF59E0B,
        .title_bg   = 0xFF33220A,
        .body_bg    = 0xFF12151E
    };

    // 4. Local LLM & Hypervisor Telemetry HUD: Compact floating window (bottom-left)
    g_wins[WIN_AI_HUD] = (SpatialWindow){
        .id = WIN_AI_HUD,
        .title = "SPATIAL AI BRIDGE & TELEMETRY (127.0.0.1:11434)",
        .x = 24, .y = 446, .w = 544, .h = 310,
        .min_w = 360, .min_h = 220,
        .border_col = 0xFFA855F7,
        .title_bg   = 0xFF28153E,
        .body_bg    = 0xFF11131F
    };

    g_z_order[0] = WIN_FILES;
    g_z_order[1] = WIN_AI_HUD;
    g_z_order[2] = WIN_MACOS;
    g_z_order[3] = WIN_LINUX;
}

static int front_window_id(void) {
    return g_z_order[WIN_COUNT - 1];
}

static void bring_window_to_front(int win_id) {
    int pos = -1;
    for (int i = 0; i < WIN_COUNT; i++) {
        if (g_z_order[i] == win_id) { pos = i; break; }
    }
    if (pos < 0 || pos == WIN_COUNT - 1) return;
    for (int i = pos; i < WIN_COUNT - 1; i++) {
        g_z_order[i] = g_z_order[i + 1];
    }
    g_z_order[WIN_COUNT - 1] = win_id;
}

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
        memmove(tb->lines[0], tb->lines[1], (TERM_HIST - 1) * (TERM_MAX_COLS + 1));
    }
    memset(tb->lines[tb->total_lines - 1], 0, TERM_MAX_COLS + 1);
    tb->cur_col = 0;
    if (tb->scroll_back > 0 && tb->scroll_back < tb->total_lines - 8) {
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
        if (tb->cur_col >= TERM_MAX_COLS) {
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
    struct winsize ws = { .ws_row = 24, .ws_col = 80 };
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
    const char *init_cmd = "uname -m -s -r && ls -la ~/zero_os/shared\n";
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
    bring_window_to_front(WIN_LINUX);
    snprintf(g_status, sizeof(g_status), "BOOTING ARM64 LINUX KERNEL + VIRTIOFS IN SPATIAL WINDOW...");

    [g_vz_vm startWithCompletionHandler:^(NSError *err) {
        pthread_mutex_lock(&g_lock);
        if (err) {
            g_vm_running = 0;
            snprintf(g_status, sizeof(g_status), "VM START ERROR: %s", [[err localizedDescription] UTF8String]);
        } else {
            g_vm_running = 1;
            snprintf(g_status, sizeof(g_status), "SPATIAL MULTI-KERNEL ACTIVE: LINUX + MACOS + VIRTIOFS COEXISTING");
            play_pcm_chime();
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                const char *welcome = "/bin/busybox --install -s 2>/dev/null; export PATH=/bin:/sbin:/usr/bin:/usr/sbin; mkdir -p /mnt/macos && mount -t virtiofs zero_share /mnt/macos && ls -la /mnt/macos\n";
                if (g_vm_in_fd >= 0) write(g_vm_in_fd, welcome, strlen(welcome));
            });
        }
        pthread_mutex_unlock(&g_lock);
    }];
#else
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
        snprintf(g_status, sizeof(g_status), "DECODED LIVE JPEG (%lu BYTES) INTO SPATIAL HUD", (unsigned long)data.length);
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

// --- .atom Binary State Crystallization (Includes Window Positions & Z-Order) ---
static void save_atom_snapshot(void) {
    ZeroAtomSnapshot *snap = calloc(1, sizeof(ZeroAtomSnapshot));
    snap->magic = 0x5A455241;
    memcpy(snap->wins, g_wins, sizeof(g_wins));
    memcpy(snap->z_order, g_z_order, sizeof(g_z_order));
    strncpy(snap->cwd, g_cwd, sizeof(snap->cwd) - 1);
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
        snprintf(g_status, sizeof(g_status), "CRYSTALLIZED SPATIAL LAYOUT + STATE -> state.atom (%lu KB)", sizeof(ZeroAtomSnapshot) / 1024);
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
    if (fread(snap, sizeof(ZeroAtomSnapshot), 1, f) == 1 && snap->magic == 0x5A455241) {
        memcpy(g_wins, snap->wins, sizeof(g_wins));
        memcpy(g_z_order, snap->z_order, sizeof(g_z_order));
        scan_directory(snap->cwd);
        strncpy(g_ai_reply, snap->ai_reply, sizeof(g_ai_reply) - 1);
        g_ai_scroll = 0;
        g_web_w = snap->img_w;
        g_web_h = snap->img_h;
        memcpy(g_web_img, snap->img_buf, sizeof(g_web_img));
        snprintf(g_status, sizeof(g_status), "INSTANTLY RESTORED SPATIAL WINDOWS & STATE IN <1 MS");
        play_pcm_chime();
    } else {
        snprintf(g_status, sizeof(g_status), "OLD .ATOM FORMAT DETECTED — PRESS [F6] TO SAVE v0.4.0 STATE");
    }
    fclose(f);
    free(snap);
}

// --- Direct Framebuffer Drawing Helpers ---
static void fill_rect(int x, int y, int w, int h, uint32_t col) {
    int x0 = MAX(0, x), y0 = MAX(0, y);
    int x1 = MIN(FB_W, x + w), y1 = MIN(FB_H, y + h);
    for (int j = y0; j < y1; j++) {
        for (int i = x0; i < x1; i++) {
            g_fb[j * FB_W + i] = col;
        }
    }
}

static void draw_border(int x, int y, int w, int h, uint32_t col, int thick) {
    fill_rect(x, y, w, thick, col);
    fill_rect(x, y + h - thick, w, thick, col);
    fill_rect(x, y, thick, h, col);
    fill_rect(x + w - thick, y, thick, h, col);
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

static int wrap_ai_lines(const char *text, int cols, char lines[AI_MAX_LINES][AI_MAX_COLS + 1]) {
    if (cols < 16) cols = 16;
    if (cols > AI_MAX_COLS) cols = AI_MAX_COLS;
    int count = 0;
    int col = 0;
    memset(lines, 0, AI_MAX_LINES * (AI_MAX_COLS + 1));
    for (size_t i = 0; text[i] != '\0' && count < AI_MAX_LINES; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') {
            count++;
            col = 0;
        } else if (c >= 32 && c < 127) {
            if (col >= cols) {
                count++;
                col = 0;
                if (count >= AI_MAX_LINES) break;
            }
            lines[count][col++] = (char)c;
        }
    }
    return count + 1;
}

static void render_terminal_window(SpatialWindow *w, TermBuffer *tb, BOOL is_front, NSColor *accent, NSColor *textCol) {
    int bx = w->x + 10;
    int by = w->y + 34;
    int bw = w->w - 28;
    int bh = w->h - 44;
    int vis_rows = MAX(3, bh / 16);
    int vis_cols = MAX(16, bw / 7);

    int max_scroll = MAX(0, tb->total_lines - vis_rows);
    if (tb->scroll_back > max_scroll) tb->scroll_back = max_scroll;
    if (tb->scroll_back < 0) tb->scroll_back = 0;

    int first_row = MAX(0, tb->total_lines - vis_rows - tb->scroll_back);
    for (int r = 0; r < vis_rows && (first_row + r) < tb->total_lines; r++) {
        if (tb->lines[first_row + r][0]) {
            char clipped[TERM_MAX_COLS + 1];
            strncpy(clipped, tb->lines[first_row + r], MIN(vis_cols, TERM_MAX_COLS));
            clipped[MIN(vis_cols, TERM_MAX_COLS)] = '\0';
            draw_text(bx, by + r * 16, clipped, 11.5, textCol, NO);
        }
    }

    // [COPY] button in title bar
    int cbx = w->x + w->w - 64;
    int cby = w->y + 5;
    fill_rect(cbx, cby, 54, 18, 0xFF0F172A);
    draw_border(cbx, cby, 54, 18, w->border_col, 1);
    draw_text(cbx + 7, cby + 2, "[COPY]", 10.5, accent, YES);

    // Scrollbar track & thumb on right edge
    int tx = w->x + w->w - 14;
    int ty = w->y + 34;
    int tw = 8;
    int th = MAX(20, w->h - 54);
    fill_rect(tx, ty, tw, th, 0xFF1E293B);
    int thumb_h = (tb->total_lines <= vis_rows) ? th : MAX(18, (th * vis_rows) / tb->total_lines);
    int thumb_y = ty + (th - thumb_h);
    if (max_scroll > 0) {
        thumb_y = ty + ((th - thumb_h) * (max_scroll - tb->scroll_back)) / max_scroll;
    }
    fill_rect(tx, thumb_y, tw, thumb_h, w->border_col);
}

static void render_files_window(SpatialWindow *w, NSColor *cCyan, NSColor *cAmber, NSColor *cWhite, NSColor *cDim) {
    char cwd_disp[128];
    int max_path_chars = MAX(20, (w->w - 30) / 7);
    snprintf(cwd_disp, sizeof(cwd_disp), "PATH: %.*s", max_path_chars, g_cwd);
    draw_text(w->x + 12, w->y + 34, cwd_disp, 11, cDim, NO);

    int list_y = w->y + 54;
    int list_h = MAX(40, w->h - 66);
    int max_vis = MAX(2, list_h / 19);
    int start_idx = (g_selected_file >= max_vis) ? (g_selected_file - max_vis + 1) : 0;
    int name_cols = MAX(14, (w->w - 160) / 7);

    for (int i = 0; i < max_vis && (start_idx + i) < g_file_count; i++) {
        int idx = start_idx + i;
        int ry = list_y + i * 19;
        if (idx == g_selected_file) fill_rect(w->x + 8, ry - 2, w->w - 18, 18, 0xFF26334D);
        char line[160];
        if (g_files[idx].is_dir) {
            snprintf(line, sizeof(line), "[DIR]  %-*.*s <DIR>", name_cols, name_cols, g_files[idx].name);
            draw_text(w->x + 14, ry, line, 11.5, cAmber, idx == g_selected_file);
        } else {
            snprintf(line, sizeof(line), "[FILE] %-*.*s %7lld B", name_cols, name_cols, g_files[idx].name, g_files[idx].size);
            draw_text(w->x + 14, ry, line, 11.5, cWhite, idx == g_selected_file);
        }
    }
}

static void render_ai_hud_window(SpatialWindow *w, NSColor *cCyan, NSColor *cGreen, NSColor *cAmber, NSColor *cWhite, NSColor *cDim) {
    char info1[96], info2[96];
    snprintf(info1, sizeof(info1), "MACH RAM: %.2f MB | LINUX VM: %s",
             get_mach_ram_mb(), g_vm_running ? "LIVE (2 vCPU, 256MB)" : "STANDBY");
    snprintf(info2, sizeof(info2), "VIRTIOFS: ~/zero_os/shared <-> /mnt/macos");
    draw_text(w->x + 12, w->y + 34, info1, 11.5, cGreen, YES);
    draw_text(w->x + 12, w->y + 52, info2, 11.0, cAmber, NO);

    // Optional compact live web thumbnail on top-right if wide enough
    if (g_web_w > 0 && g_web_h > 0 && w->w >= 480) {
        int tw = 108, th = 72;
        int tx = w->x + w->w - tw - 14;
        int ty = w->y + 34;
        for (int j = 0; j < th; j++) {
            int sy = (j * g_web_h) / th;
            for (int i = 0; i < tw; i++) {
                int sx = (i * g_web_w) / tw;
                int dx = tx + i, dy = ty + j;
                if (dx >= 0 && dx < FB_W && dy >= 0 && dy < FB_H) {
                    g_fb[dy * FB_W + dx] = g_web_img[sy * g_web_w + sx];
                }
            }
        }
        draw_border(tx - 1, ty - 1, tw + 2, th + 2, 0xFF38BDF8, 1);
    }

    // AI Prompt/Reply Box
    int bx = w->x + 12;
    int by = w->y + 114;
    int bw = MAX(200, w->w - 24);
    int bh = MAX(90, w->h - 128);
    fill_rect(bx, by, bw, bh, 0xFF080A10);
    draw_border(bx, by, bw, bh, g_ai_input_mode ? 0xFFF59E0B : 0xFF3B4254, 1);
    draw_text(bx + 10, by + 6, "LOCAL LLM (127.0.0.1:11434) — [F8] Ask | [Cmd+C] Copy", 11, cCyan, YES);

    if (g_ai_input_mode) {
        int pcols = MAX(18, (bw - 24) / 7);
        size_t plen = strlen(g_ai_prompt);
        const char *tail = (plen > (size_t)pcols) ? (g_ai_prompt + plen - pcols) : g_ai_prompt;
        char pbuf[220];
        snprintf(pbuf, sizeof(pbuf), "PROMPT> %s_", tail);
        draw_text(bx + 10, by + 30, pbuf, 11.5, cAmber, YES);
        draw_text(bx + 10, by + bh - 20, "[Cmd+V] Paste | [Enter] Send | [Esc] Cancel", 10.5, cDim, NO);
    } else {
        int cols = MAX(20, (bw - 34) / 7);
        int vis_rows = MAX(2, (bh - 46) / 16);
        char lines[AI_MAX_LINES][AI_MAX_COLS + 1];
        g_ai_total_lines = wrap_ai_lines(g_ai_reply, cols, lines);
        int max_scroll = MAX(0, g_ai_total_lines - vis_rows);
        if (g_ai_scroll > max_scroll) g_ai_scroll = max_scroll;
        if (g_ai_scroll < 0) g_ai_scroll = 0;

        for (int r = 0; r < vis_rows; r++) {
            int lidx = g_ai_scroll + r;
            if (lidx < g_ai_total_lines && lines[lidx][0]) {
                draw_text(bx + 10, by + 26 + r * 16, lines[lidx], 11.5, cWhite, NO);
            }
        }

        // Scrollbar inside AI box
        int tx = bx + bw - 14;
        int ty = by + 25;
        int tw = 8;
        int th = MAX(16, bh - 44);
        fill_rect(tx, ty, tw, th, 0xFF1E293B);
        int thumb_h = (g_ai_total_lines <= vis_rows) ? th : MAX(12, (th * vis_rows) / g_ai_total_lines);
        int thumb_y = ty;
        if (max_scroll > 0) {
            thumb_y = ty + ((th - thumb_h) * g_ai_scroll) / max_scroll;
        }
        fill_rect(tx, thumb_y, tw, thumb_h, 0xFFA855F7);

        char hint[96];
        snprintf(hint, sizeof(hint), "Line %d/%d | Drag title to move window, corner to resize",
                 MIN(g_ai_scroll + vis_rows, g_ai_total_lines), g_ai_total_lines);
        draw_text(bx + 10, by + bh - 18, hint, 10, cDim, NO);
    }
}

static void render_spatial_window(SpatialWindow *w, BOOL is_front) {
    NSColor *cCyan  = [NSColor colorWithRed:0.25 green:0.90 blue:0.95 alpha:1.0];
    NSColor *cGreen = [NSColor colorWithRed:0.30 green:0.95 blue:0.55 alpha:1.0];
    NSColor *cAmber = [NSColor colorWithRed:1.00 green:0.78 blue:0.25 alpha:1.0];
    NSColor *cWhite = [NSColor colorWithWhite:0.92 alpha:1.0];
    NSColor *cDim   = [NSColor colorWithWhite:0.58 alpha:1.0];

    // Drop shadow for spatial depth
    fill_rect(w->x + 6, w->y + 6, w->w, w->h, 0xFF04060A);

    // Window body & title bar
    fill_rect(w->x, w->y, w->w, w->h, w->body_bg);
    fill_rect(w->x, w->y, w->w, 28, is_front ? w->title_bg : 0xFF161B26);
    draw_border(w->x, w->y, w->w, w->h, is_front ? w->border_col : 0xFF334155, is_front ? 2 : 1);

    // Clip all window interior drawing strictly inside the window bounds
    CGContextSaveGState(g_ctx);
    CGContextClipToRect(g_ctx, CGRectMake(w->x + 2, w->y + 2, w->w - 4, w->h - 4));

    // Active indicator dot + Title
    fill_rect(w->x + 10, w->y + 10, 8, 8, is_front ? w->border_col : 0xFF475569);
    draw_text(w->x + 24, w->y + 6, w->title, 11.5, is_front ? cWhite : cDim, is_front);

    if (w->id == WIN_LINUX) {
        render_terminal_window(w, &g_vm_term, is_front, cCyan, cGreen);
    } else if (w->id == WIN_MACOS) {
        render_terminal_window(w, &g_host_term, is_front, cGreen, cWhite);
    } else if (w->id == WIN_FILES) {
        render_files_window(w, cCyan, cAmber, cWhite, cDim);
    } else if (w->id == WIN_AI_HUD) {
        render_ai_hud_window(w, cCyan, cGreen, cAmber, cWhite, cDim);
    }

    // Bottom-right resize grip triangle
    int gx = w->x + w->w - 14;
    int gy = w->y + w->h - 14;
    for (int k = 0; k < 10; k += 3) {
        fill_rect(gx + k, gy + 10 - k, 10 - k, 2, is_front ? w->border_col : 0xFF475569);
    }

    CGContextRestoreGState(g_ctx);
}

static void render_frame(void) {
    pthread_mutex_lock(&g_lock);

    // Spatial dark canvas background with subtle grid dots
    fill_rect(0, 0, FB_W, FB_H, 0xFF070A10);
    for (int gy = 24; gy < FB_H - 24; gy += 32) {
        for (int gx = 24; gx < FB_W; gx += 32) {
            g_fb[gy * FB_W + gx] = 0xFF161E2E;
        }
    }

    NSGraphicsContext *prevCtx = [NSGraphicsContext currentContext];
    NSGraphicsContext *nsCtx = [NSGraphicsContext graphicsContextWithCGContext:g_ctx flipped:YES];
    [NSGraphicsContext setCurrentContext:nsCtx];

    // Render all spatial windows from back to front (Z-order)
    for (int i = 0; i < WIN_COUNT; i++) {
        int wid = g_z_order[i];
        BOOL is_front = (i == WIN_COUNT - 1);
        render_spatial_window(&g_wins[wid], is_front);
    }

    // Bottom Spatial Dock / Status Bar
    NSColor *cGreen = [NSColor colorWithRed:0.30 green:0.95 blue:0.55 alpha:1.0];
    NSColor *cCyan  = [NSColor colorWithRed:0.25 green:0.90 blue:0.95 alpha:1.0];
    fill_rect(0, FB_H - 24, FB_W, 24, 0xFF0F172A);
    fill_rect(0, FB_H - 24, FB_W, 1, 0xFF1E293B);
    draw_text(14, FB_H - 20, g_status, 11, cGreen, YES);
    draw_text(740, FB_H - 20, "[F1] Next Win | [F3] Shared | [F6/F7] .atom | [F10] Reset Layout", 10.5, cCyan, NO);

    [NSGraphicsContext setCurrentContext:prevCtx];
    pthread_mutex_unlock(&g_lock);
}

@interface ZeroView : NSView {
    int _dragMode; // 0=none, 1=move window, 2=resize window, 3=AI scroll, 4=Term scroll
    int _dragWinId;
    int _dragOffX, _dragOffY;
    int _origW, _origH;
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

- (int)hitTestWindowAtX:(int)fx y:(int)fy {
    for (int i = WIN_COUNT - 1; i >= 0; i--) {
        int wid = g_z_order[i];
        SpatialWindow *w = &g_wins[wid];
        if (fx >= w->x && fx <= w->x + w->w && fy >= w->y && fy <= w->y + w->h) {
            return wid;
        }
    }
    return -1;
}

- (void)updateTermScroll:(int)wid fromY:(CGFloat)fy {
    SpatialWindow *w = &g_wins[wid];
    TermBuffer *tb = (wid == WIN_LINUX) ? &g_vm_term : &g_host_term;
    int vis_rows = MAX(3, (w->h - 44) / 16);
    int max_scroll = MAX(0, tb->total_lines - vis_rows);
    int ty = w->y + 34;
    int th = MAX(20, w->h - 54);
    if (max_scroll > 0) {
        CGFloat ratio = (fy - ty) / (CGFloat)th;
        if (ratio < 0.0) ratio = 0.0;
        if (ratio > 1.0) ratio = 1.0;
        tb->scroll_back = (int)round((1.0 - ratio) * max_scroll);
    }
}

- (void)updateAiScrollFromY:(CGFloat)fy {
    SpatialWindow *w = &g_wins[WIN_AI_HUD];
    int by = w->y + 114;
    int bh = MAX(90, w->h - 128);
    int vis_rows = MAX(2, (bh - 46) / 16);
    int max_scroll = MAX(0, g_ai_total_lines - vis_rows);
    int ty = by + 25;
    int th = MAX(16, bh - 44);
    if (max_scroll > 0) {
        CGFloat ratio = (fy - ty) / (CGFloat)th;
        if (ratio < 0.0) ratio = 0.0;
        if (ratio > 1.0) ratio = 1.0;
        g_ai_scroll = (int)round(ratio * max_scroll);
    }
}

- (void)copyConsoleLog:(int)wid {
    TermBuffer *tb = (wid == WIN_LINUX) ? &g_vm_term : &g_host_term;
    NSMutableString *out = [NSMutableString string];
    for (int i = 0; i < tb->total_lines; i++) {
        NSString *line = [NSString stringWithUTF8String:tb->lines[i]];
        if (line) [out appendFormat:@"%@\n", line];
    }
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:out forType:NSPasteboardTypeString];
    snprintf(g_status, sizeof(g_status), "COPIED %s LOG (%d LINES) TO CLIPBOARD",
             (wid == WIN_LINUX) ? "LINUX KERNEL" : "MACOS SHELL", tb->total_lines);
}

- (void)mouseDown:(NSEvent *)event {
    NSPoint fp = [self fbPointFromEvent:event];
    int fx = (int)fp.x, fy = (int)fp.y;
    _dragMode = 0;
    _dragWinId = -1;

    pthread_mutex_lock(&g_lock);
    int wid = [self hitTestWindowAtX:fx y:fy];
    if (wid >= 0) {
        bring_window_to_front(wid);
        SpatialWindow *w = &g_wins[wid];
        _dragWinId = wid;

        // Check [COPY] button on terminal windows
        if ((wid == WIN_LINUX || wid == WIN_MACOS) &&
            fx >= w->x + w->w - 66 && fx <= w->x + w->w - 8 &&
            fy >= w->y + 4 && fy <= w->y + 24) {
            [self copyConsoleLog:wid];
            pthread_mutex_unlock(&g_lock);
            return;
        }

        // Check title bar drag (top 28px)
        if (fy <= w->y + 28) {
            _dragMode = 1;
            _dragOffX = fx - w->x;
            _dragOffY = fy - w->y;
            g_ai_input_mode = 0;
            pthread_mutex_unlock(&g_lock);
            return;
        }

        // Check bottom-right resize grip (20x20px corner)
        if (fx >= w->x + w->w - 20 && fy >= w->y + w->h - 20) {
            _dragMode = 2;
            _dragOffX = fx;
            _dragOffY = fy;
            _origW = w->w;
            _origH = w->h;
            pthread_mutex_unlock(&g_lock);
            return;
        }

        // Check terminal scrollbar drag
        if ((wid == WIN_LINUX || wid == WIN_MACOS) && fx >= w->x + w->w - 20) {
            _dragMode = 4;
            [self updateTermScroll:wid fromY:fy];
            pthread_mutex_unlock(&g_lock);
            return;
        }

        // Check AI HUD interactions
        if (wid == WIN_AI_HUD) {
            int by = w->y + 114;
            if (fy >= by) {
                if (fx >= w->x + w->w - 32) {
                    _dragMode = 3;
                    [self updateAiScrollFromY:fy];
                } else {
                    g_ai_input_mode = 1;
                    g_ai_prompt[0] = '\0';
                    snprintf(g_status, sizeof(g_status), "AI PROMPT MODE: PRESS [CMD+V] TO PASTE OR TYPE & PRESS [ENTER]");
                }
            }
        } else {
            g_ai_input_mode = 0;
            if (wid == WIN_FILES && fy >= w->y + 54) {
                int clicked = (fy - (w->y + 54)) / 19;
                int max_vis = MAX(2, (w->h - 66) / 19);
                int start_idx = (g_selected_file >= max_vis) ? (g_selected_file - max_vis + 1) : 0;
                if (start_idx + clicked < g_file_count) {
                    g_selected_file = start_idx + clicked;
                }
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
}

- (void)mouseDragged:(NSEvent *)event {
    if (_dragMode == 0 || _dragWinId < 0) return;
    NSPoint fp = [self fbPointFromEvent:event];
    int fx = (int)fp.x, fy = (int)fp.y;

    pthread_mutex_lock(&g_lock);
    SpatialWindow *w = &g_wins[_dragWinId];
    if (_dragMode == 1) {
        w->x = MAX(0, MIN(FB_W - 80, fx - _dragOffX));
        w->y = MAX(0, MIN(FB_H - 52, fy - _dragOffY));
    } else if (_dragMode == 2) {
        int nw = _origW + (fx - _dragOffX);
        int nh = _origH + (fy - _dragOffY);
        w->w = MAX(w->min_w, MIN(FB_W - w->x, nw));
        w->h = MAX(w->min_h, MIN(FB_H - 26 - w->y, nh));
    } else if (_dragMode == 3) {
        [self updateAiScrollFromY:fy];
    } else if (_dragMode == 4) {
        [self updateTermScroll:_dragWinId fromY:fy];
    }
    pthread_mutex_unlock(&g_lock);
}

- (void)mouseUp:(NSEvent *)event {
    _dragMode = 0;
    _dragWinId = -1;
}

- (void)scrollWheel:(NSEvent *)event {
    CGFloat dy = [event scrollingDeltaY];
    if (fabs(dy) < 0.1) return;
    NSPoint fp = [self fbPointFromEvent:event];

    pthread_mutex_lock(&g_lock);
    int wid = [self hitTestWindowAtX:(int)fp.x y:(int)fp.y];
    if (wid == WIN_LINUX || wid == WIN_MACOS) {
        SpatialWindow *w = &g_wins[wid];
        TermBuffer *tb = (wid == WIN_LINUX) ? &g_vm_term : &g_host_term;
        int vis_rows = MAX(3, (w->h - 44) / 16);
        int max_scroll = MAX(0, tb->total_lines - vis_rows);
        if (dy > 0) tb->scroll_back = MIN(max_scroll, tb->scroll_back + 2);
        else tb->scroll_back = MAX(0, tb->scroll_back - 2);
    } else if (wid == WIN_AI_HUD) {
        SpatialWindow *w = &g_wins[WIN_AI_HUD];
        int bh = MAX(90, w->h - 128);
        int vis_rows = MAX(2, (bh - 46) / 16);
        int max_scroll = MAX(0, g_ai_total_lines - vis_rows);
        if (dy < 0) g_ai_scroll = MIN(max_scroll, g_ai_scroll + 1);
        else g_ai_scroll = MAX(0, g_ai_scroll - 1);
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
        int fw = front_window_id();
        if (u) {
            if (fw == WIN_MACOS && g_pty_fd >= 0) {
                g_host_term.scroll_back = 0;
                write(g_pty_fd, u, strlen(u));
                snprintf(g_status, sizeof(g_status), "PASTED INTO MACOS SHELL WINDOW");
            } else if (g_vm_in_fd >= 0) {
                bring_window_to_front(WIN_LINUX);
                g_vm_term.scroll_back = 0;
                write(g_vm_in_fd, u, strlen(u));
                snprintf(g_status, sizeof(g_status), "PASTED INTO LINUX KERNEL WINDOW");
            }
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
            pthread_mutex_lock(&g_lock);
            int fw = front_window_id();
            if ((mods & NSEventModifierFlagShift) && (fw == WIN_LINUX || fw == WIN_MACOS)) {
                [self copyConsoleLog:fw];
                pthread_mutex_unlock(&g_lock);
            } else {
                pthread_mutex_unlock(&g_lock);
                [self copyAiReplyToClipboard];
            }
            return;
        }
    }

    if (kc == 122) { // F1 -> Cycle frontmost spatial window
        pthread_mutex_lock(&g_lock);
        int back_id = g_z_order[0];
        bring_window_to_front(back_id);
        g_ai_input_mode = (back_id == WIN_AI_HUD) ? 0 : 0;
        snprintf(g_status, sizeof(g_status), "ACTIVE WINDOW: %s", g_wins[back_id].title);
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (kc == 120) { // F2 -> Open selected directory
        if (g_selected_file < g_file_count && g_files[g_selected_file].is_dir) {
            char next[1024];
            snprintf(next, sizeof(next), "%s/%s", g_cwd, g_files[g_selected_file].name);
            char resolved[512];
            if (realpath(next, resolved)) {
                scan_directory(resolved);
                bring_window_to_front(WIN_FILES);
            }
        }
        return;
    }
    if (kc == 99) { // F3 -> Jump to VirtioFS shared directory
        char shared[512];
        snprintf(shared, sizeof(shared), "%s/zero_os/shared", getenv("HOME"));
        scan_directory(shared);
        bring_window_to_front(WIN_FILES);
        snprintf(g_status, sizeof(g_status), "VIRTIOFS SHARED DIR: ~/zero_os/shared <-> /mnt/macos");
        return;
    }
    if (kc == 118) { boot_native_linux_kernel(); return; }    // F4
    if (kc == 96)  { fetch_live_web_image(); return; }        // F5
    if (kc == 97)  { save_atom_snapshot(); return; }          // F6
    if (kc == 98)  { restore_atom_snapshot(); return; }       // F7
    if (kc == 100 || (kc == 53 && g_ai_input_mode)) {         // F8 or Esc
        g_ai_input_mode = !g_ai_input_mode;
        g_ai_prompt[0] = '\0';
        if (g_ai_input_mode) bring_window_to_front(WIN_AI_HUD);
        snprintf(g_status, sizeof(g_status), g_ai_input_mode ? "AI PROMPT MODE: PRESS [CMD+V] TO PASTE OR TYPE & PRESS [ENTER]" : "EXITED AI PROMPT MODE");
        return;
    }
    if (kc == 101) { play_pcm_chime(); return; }              // F9
    if (kc == 109) {                                          // F10 -> Reset spatial layout
        pthread_mutex_lock(&g_lock);
        reset_spatial_layout();
        snprintf(g_status, sizeof(g_status), "RESET SPATIAL WINDOWS TO DEFAULT MULTI-OS LAYOUT");
        pthread_mutex_unlock(&g_lock);
        return;
    }

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

    // Route keyboard input to focused OS window (Linux or macOS)
    if (chars.length > 0) {
        const char *u = [chars UTF8String];
        if (!u) return;
        int fw = front_window_id();
        if (fw == WIN_MACOS && g_pty_fd >= 0) {
            g_host_term.scroll_back = 0;
            write(g_pty_fd, u, strlen(u));
        } else if (g_vm_in_fd >= 0) {
            if (fw != WIN_LINUX) bring_window_to_front(WIN_LINUX);
            g_vm_term.scroll_back = 0;
            write(g_vm_in_fd, u, strlen(u));
        }
    }
}
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        reset_spatial_layout();

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
        [win setTitle:@"ZERO OS v0.4.0 — Spatial Multi-Kernel Compositor (ARM64 Linux + macOS + VirtioFS)"];
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
