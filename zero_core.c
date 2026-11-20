#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#import <ImageIO/ImageIO.h>
#import <WebKit/WebKit.h>
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
#define WIN_COUNT 5
#define TERM_HIST 240
#define TERM_MAX_COLS 120
#define MAX_FILES 64
#define AI_MAX_LINES 160
#define AI_MAX_COLS 120

enum {
    WIN_LINUX = 0,
    WIN_MACOS = 1,
    WIN_FILES = 2,
    WIN_AI_HUD = 3,
    WIN_BROWSER = 4
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
    char browser_url[512];
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
static int g_z_order[WIN_COUNT] = { WIN_FILES, WIN_AI_HUD, WIN_MACOS, WIN_LINUX, WIN_BROWSER };

// Native WebKit Spatial Stream & Live DOM Perception
static WKWebView *g_webView = nil;
static char g_browser_url[512] = "https://github.com/sydneyminato/zero-os";
static char g_url_input[512] = "";
static int g_url_input_mode = 0;
static uint32_t *g_browser_snap = NULL;
static int g_snap_w = 0, g_snap_h = 0;
static int g_snap_tick = 0;
static char g_web_title_cache[256] = "";
static char g_web_text_cache[1600] = "";

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
static char g_status[256] = "ZERO OS v0.7.0: CLOSED-LOOP AL AGENT + LIVE WEBKIT DOM PERCEPTION";

// Closed-Loop AL (Autonomous Layer) Agent State
static char g_ai_model[64] = "llama3.2";
static int g_ai_auto_exec = 1;
static int g_al_step = 0;
static int g_al_max_steps = 3;
static int g_al_abort = 0;
static char g_al_goal[1024] = "";
static char g_ai_last_action[128] = "AL LOOP IDLE (WebKit Reader + Self-Heal Armed)";
static char g_ai_reply[4096] = "Click AI box or [F8], paste instruction ([Cmd+V]) & [Enter].\nAL Loop reads live WebKit pages, checks exit codes, and self-heals up to 3 steps.\n[F11] Toggle Auto-Exec | [F12] Rollback zero_auto.atom | [Esc] Abort Loop";
static int g_ai_scroll = 0;
static int g_ai_total_lines = 1;
static int g_ai_input_mode = 0;
static char g_ai_prompt[1024] = "";

// Forward declarations
static void run_al_step(int step, const char *observation);
static void capture_web_dom_text(void (^done)(void));
static void save_atom_snapshot_file(const char *filename, BOOL silent);
static void restore_atom_snapshot_file(const char *filename);

// --- Spatial Window Layout Initialization ---
static void reset_spatial_layout(void) {
    g_wins[WIN_LINUX] = (SpatialWindow){
        .id = WIN_LINUX,
        .title = "GUEST LINUX KERNEL (ARM64 HYPERVISOR | /mnt/macos)",
        .x = 20, .y = 20, .w = 560, .h = 360,
        .min_w = 340, .min_h = 190,
        .border_col = 0xFF38BDF8,
        .title_bg   = 0xFF102A43,
        .body_bg    = 0xFF0B111A
    };

    g_wins[WIN_MACOS] = (SpatialWindow){
        .id = WIN_MACOS,
        .title = "HOST MACOS SHELL (POSIX /bin/zsh)",
        .x = 460, .y = 420, .w = 500, .h = 340,
        .min_w = 320, .min_h = 180,
        .border_col = 0xFF30F28C,
        .title_bg   = 0xFF123524,
        .body_bg    = 0xFF0C1410
    };

    g_wins[WIN_FILES] = (SpatialWindow){
        .id = WIN_FILES,
        .title = "VIRTIOFS FILES ([O] Open | [F3] Shared)",
        .x = 976, .y = 440, .w = 284, .h = 320,
        .min_w = 250, .min_h = 180,
        .border_col = 0xFFF59E0B,
        .title_bg   = 0xFF33220A,
        .body_bg    = 0xFF12151E
    };

    g_wins[WIN_AI_HUD] = (SpatialWindow){
        .id = WIN_AI_HUD,
        .title = "AUTONOMOUS AL AGENT (CLOSED-LOOP PERCEPTION)",
        .x = 20, .y = 400, .w = 424, .h = 360,
        .min_w = 320, .min_h = 210,
        .border_col = 0xFFA855F7,
        .title_bg   = 0xFF28153E,
        .body_bg    = 0xFF11131F
    };

    g_wins[WIN_BROWSER] = (SpatialWindow){
        .id = WIN_BROWSER,
        .title = "SPATIAL WEB BROWSER (NATIVE WEBKIT STREAM)",
        .x = 600, .y = 20, .w = 660, .h = 400,
        .min_w = 360, .min_h = 240,
        .border_col = 0xFFEC4899,
        .title_bg   = 0xFF3B1228,
        .body_bg    = 0xFF0F131D
    };

    g_z_order[0] = WIN_FILES;
    g_z_order[1] = WIN_AI_HUD;
    g_z_order[2] = WIN_MACOS;
    g_z_order[3] = WIN_LINUX;
    g_z_order[4] = WIN_BROWSER;
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

static BOOL is_browser_occluded(void) {
    int bpos = -1;
    for (int i = 0; i < WIN_COUNT; i++) {
        if (g_z_order[i] == WIN_BROWSER) { bpos = i; break; }
    }
    SpatialWindow *bw = &g_wins[WIN_BROWSER];
    for (int i = bpos + 1; i < WIN_COUNT; i++) {
        SpatialWindow *ow = &g_wins[g_z_order[i]];
        BOOL overlapX = (bw->x < ow->x + ow->w) && (bw->x + bw->w > ow->x);
        BOOL overlapY = (bw->y < ow->y + ow->h) && (bw->y + bw->h > ow->y);
        if (overlapX && overlapY) return YES;
    }
    return NO;
}

static void navigate_browser_to(const char *raw_url) {
    if (!raw_url || !raw_url[0]) return;
    char full[512];
    if (strstr(raw_url, "://") != NULL) {
        snprintf(full, sizeof(full), "%s", raw_url);
    } else if (strchr(raw_url, '.') != NULL && strchr(raw_url, ' ') == NULL) {
        snprintf(full, sizeof(full), "https://%s", raw_url);
    } else {
        NSString *rawNs = [NSString stringWithUTF8String:raw_url];
        NSString *q = rawNs ? [rawNs stringByAddingPercentEncodingWithAllowedCharacters:[NSCharacterSet URLQueryAllowedCharacterSet]] : @"";
        snprintf(full, sizeof(full), "https://duckduckgo.com/?q=%s", [q UTF8String]);
    }
    strncpy(g_browser_url, full, sizeof(g_browser_url) - 1);
    g_browser_url[sizeof(g_browser_url) - 1] = '\0';
    NSString *nsUrl = [NSString stringWithUTF8String:g_browser_url];
    if (nsUrl && g_webView) {
        NSURL *u = [NSURL URLWithString:nsUrl];
        if (u) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [g_webView loadRequest:[NSURLRequest requestWithURL:u]];
            });
            snprintf(g_status, sizeof(g_status), "WEB STREAM: %s", g_browser_url);
        }
    }
}

// --- Live WebKit DOM Text Perception ---
static void capture_web_dom_text(void (^done)(void)) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!g_webView) {
            if (done) done();
            return;
        }
        NSString *js = @"(()=>{let c=document.body?document.body.cloneNode(true):null;if(!c)return JSON.stringify({title:document.title||'',text:''});c.querySelectorAll('nav,header,footer,script,style,noscript').forEach(e=>e.remove());let t=(c.innerText||'').replace(/skip to content/gi,'').replace(/\\s+/g,' ').trim().slice(0,1400);return JSON.stringify({title:document.title||'',text:t});})()";
        [g_webView evaluateJavaScript:js completionHandler:^(id res, NSError *err) {
            if (!err && [res isKindOfClass:[NSString class]]) {
                NSData *d = [(NSString *)res dataUsingEncoding:NSUTF8StringEncoding];
                if (d) {
                    NSDictionary *obj = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
                    if ([obj isKindOfClass:[NSDictionary class]]) {
                        NSString *t = obj[@"title"];
                        NSString *b = obj[@"text"];
                        pthread_mutex_lock(&g_lock);
                        if ([t isKindOfClass:[NSString class]]) {
                            strncpy(g_web_title_cache, [t UTF8String] ?: "", sizeof(g_web_title_cache) - 1);
                            g_web_title_cache[sizeof(g_web_title_cache) - 1] = '\0';
                        }
                        if ([b isKindOfClass:[NSString class]]) {
                            strncpy(g_web_text_cache, [b UTF8String] ?: "", sizeof(g_web_text_cache) - 1);
                            g_web_text_cache[sizeof(g_web_text_cache) - 1] = '\0';
                        }
                        pthread_mutex_unlock(&g_lock);
                    }
                }
            }
            if (done) done();
        }];
    });
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
    snprintf(g_status, sizeof(g_status), "BOOTING ARM64 LINUX KERNEL + VIRTIOFS IN SPATIAL WINDOW...");

    [g_vz_vm startWithCompletionHandler:^(NSError *err) {
        pthread_mutex_lock(&g_lock);
        if (err) {
            g_vm_running = 0;
            snprintf(g_status, sizeof(g_status), "VM START ERROR: %s", [[err localizedDescription] UTF8String]);
        } else {
            g_vm_running = 1;
            snprintf(g_status, sizeof(g_status), "ZERO OS v0.7.0: CLOSED-LOOP AL AGENT + LINUX + MACOS + WEBKIT ACTIVE");
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

    [[[NSURLSession sharedSession] dataTaskWithURL:url completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
        if (err || !data) return;
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
        pthread_mutex_unlock(&g_lock);
    }] resume];
}

// --- Closed-Loop AL (Autonomous Layer) Perception-Action Engine ---
static NSString *extract_tag_block(NSString *text, NSString *openTag, NSString *closeTag) {
    NSRange start = [text rangeOfString:openTag options:NSCaseInsensitiveSearch];
    if (start.location == NSNotFound) return nil;
    NSUInteger contentStart = start.location + start.length;
    NSRange searchRange = NSMakeRange(contentStart, text.length - contentStart);
    NSRange end = [text rangeOfString:closeTag options:NSCaseInsensitiveSearch range:searchRange];
    if (end.location == NSNotFound) return nil;
    NSString *raw = [text substringWithRange:NSMakeRange(contentStart, end.location - contentStart)];
    return [raw stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

static void detect_ollama_model(void) {
    NSURL *url = [NSURL URLWithString:@"http://127.0.0.1:11434/api/tags"];
    [[[NSURLSession sharedSession] dataTaskWithURL:url completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
        if (err || !data) return;
        NSDictionary *json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        NSArray *models = json[@"models"];
        if ([models isKindOfClass:[NSArray class]] && models.count > 0) {
            NSString *first = models[0][@"name"];
            for (NSDictionary *m in models) {
                NSString *n = m[@"name"];
                if ([n containsString:@"llama3.2"] || [n containsString:@"qwen"] || [n containsString:@"gemma"]) {
                    first = n;
                    break;
                }
            }
            if (first) {
                pthread_mutex_lock(&g_lock);
                strncpy(g_ai_model, [first UTF8String], sizeof(g_ai_model) - 1);
                pthread_mutex_unlock(&g_lock);
            }
        }
    }] resume];
}

static NSString *harvest_execution_logs(BOOL *outHasError) {
    NSString *sharedDir = [NSHomeDirectory() stringByAppendingPathComponent:@"zero_os/shared"];
    NSString *linLog = [NSString stringWithContentsOfFile:[sharedDir stringByAppendingPathComponent:@"ai_result.txt"] encoding:NSUTF8StringEncoding error:nil];
    NSString *macLog = [NSString stringWithContentsOfFile:[sharedDir stringByAppendingPathComponent:@"ai_macos_result.txt"] encoding:NSUTF8StringEncoding error:nil];

    NSMutableString *obs = [NSMutableString string];
    BOOL errFlag = NO;

    if (linLog.length > 0) {
        NSString *snip = (linLog.length > 600) ? [linLog substringFromIndex:linLog.length - 600] : linLog;
        [obs appendFormat:@"[LINUX_OUTPUT]\n%@\n", snip];
        if (([snip rangeOfString:@"LINUX_EXIT:"].location != NSNotFound && [snip rangeOfString:@"LINUX_EXIT:0"].location == NSNotFound) ||
            [snip rangeOfString:@"can't " options:NSCaseInsensitiveSearch].location != NSNotFound ||
            [snip rangeOfString:@"not found" options:NSCaseInsensitiveSearch].location != NSNotFound ||
            [snip rangeOfString:@"No such file" options:NSCaseInsensitiveSearch].location != NSNotFound ||
            [snip rangeOfString:@"syntax error" options:NSCaseInsensitiveSearch].location != NSNotFound) {
            errFlag = YES;
        }
    }
    if (macLog.length > 0) {
        NSString *snip = (macLog.length > 600) ? [macLog substringFromIndex:macLog.length - 600] : macLog;
        [obs appendFormat:@"[MACOS_OUTPUT]\n%@\n", snip];
        if (([snip rangeOfString:@"MACOS_EXIT:"].location != NSNotFound && [snip rangeOfString:@"MACOS_EXIT:0"].location == NSNotFound) ||
            [snip rangeOfString:@"No such file" options:NSCaseInsensitiveSearch].location != NSNotFound ||
            [snip rangeOfString:@"command not found" options:NSCaseInsensitiveSearch].location != NSNotFound) {
            errFlag = YES;
        }
    }
    if (outHasError) *outHasError = errFlag;
    return obs;
}

static void dispatch_al_step_response(int step, NSString *rawResponse) {
    NSString *linuxCmd  = extract_tag_block(rawResponse, @"[LINUX]", @"[/LINUX]");
    NSString *macosCmd  = extract_tag_block(rawResponse, @"[MACOS]", @"[/MACOS]");
    NSString *webTarget = extract_tag_block(rawResponse, @"[WEB]", @"[/WEB]");
    NSString *spaceWin  = extract_tag_block(rawResponse, @"[SPACE]", @"[/SPACE]");
    NSString *statusTag = extract_tag_block(rawResponse, @"[STATUS]", @"[/STATUS]");

    NSString *sharedDir = [NSHomeDirectory() stringByAppendingPathComponent:@"zero_os/shared"];
    [[NSFileManager defaultManager] createDirectoryAtPath:sharedDir withIntermediateDirectories:YES attributes:nil error:nil];

    [[NSFileManager defaultManager] removeItemAtPath:[sharedDir stringByAppendingPathComponent:@"ai_result.txt"] error:nil];
    [[NSFileManager defaultManager] removeItemAtPath:[sharedDir stringByAppendingPathComponent:@"ai_macos_result.txt"] error:nil];

    NSMutableArray *actionsDone = [NSMutableArray array];
    BOOL ranLinux = NO, ranMacos = NO, ranWeb = NO;

    if (linuxCmd.length > 0 && g_ai_auto_exec) {
        linuxCmd = [linuxCmd stringByReplacingOccurrencesOfString:@"~/zero_os/shared" withString:@"/mnt/macos"];
        linuxCmd = [linuxCmd stringByReplacingOccurrencesOfString:@"$HOME/zero_os/shared" withString:@"/mnt/macos"];
        linuxCmd = [linuxCmd stringByReplacingOccurrencesOfString:@"/mnt/macos/shared" withString:@"/mnt/macos"];

        NSString *scriptPath = [sharedDir stringByAppendingPathComponent:@"ai_task.sh"];
        NSString *scriptBody = [NSString stringWithFormat:
            @"#!/bin/sh\necho '=== [AL STEP %d: LINUX] ==='\n%@\nEC=$?\necho \"=== [LINUX_EXIT:$EC] ===\"\n",
            step, linuxCmd];
        [scriptBody writeToFile:scriptPath atomically:YES encoding:NSUTF8StringEncoding error:nil];
        chmod([scriptPath UTF8String], 0755);

        if (g_vm_in_fd >= 0 && g_vm_running) {
            const char *execLine = "sh /mnt/macos/ai_task.sh > /mnt/macos/ai_result.txt 2>&1; cat /mnt/macos/ai_result.txt\n";
            g_vm_term.scroll_back = 0;
            write(g_vm_in_fd, execLine, strlen(execLine));
            bring_window_to_front(WIN_LINUX);
            [actionsDone addObject:@"Linux"];
            ranLinux = YES;
        }
    }

    if (macosCmd.length > 0 && g_ai_auto_exec) {
        NSString *macScript = [sharedDir stringByAppendingPathComponent:@"ai_macos.sh"];
        NSString *macBody = [NSString stringWithFormat:
            @"#!/bin/zsh\necho '=== [AL STEP %d: MACOS] ==='\n%@\nEC=$?\necho \"=== [MACOS_EXIT:$EC] ===\"\n",
            step, macosCmd];
        [macBody writeToFile:macScript atomically:YES encoding:NSUTF8StringEncoding error:nil];
        chmod([macScript UTF8String], 0755);

        if (g_pty_fd >= 0) {
            const char *execMac = "zsh ~/zero_os/shared/ai_macos.sh > ~/zero_os/shared/ai_macos_result.txt 2>&1; cat ~/zero_os/shared/ai_macos_result.txt\n";
            g_host_term.scroll_back = 0;
            write(g_pty_fd, execMac, strlen(execMac));
            if (!ranLinux) bring_window_to_front(WIN_MACOS);
            [actionsDone addObject:@"macOS"];
            ranMacos = YES;
        }
    }

    if (webTarget.length > 0 && g_ai_auto_exec) {
        navigate_browser_to([webTarget UTF8String]);
        if (!ranLinux && !ranMacos) bring_window_to_front(WIN_BROWSER);
        [actionsDone addObject:@"WebKit"];
        ranWeb = YES;
    }

    if (spaceWin.length > 0 && g_ai_auto_exec) {
        NSString *u = [spaceWin uppercaseString];
        if ([u containsString:@"LINUX"]) bring_window_to_front(WIN_LINUX);
        else if ([u containsString:@"MACOS"]) bring_window_to_front(WIN_MACOS);
        else if ([u containsString:@"WEB"]) bring_window_to_front(WIN_BROWSER);
        else if ([u containsString:@"FILE"]) bring_window_to_front(WIN_FILES);
    }

    BOOL wantContinue = (statusTag && [[statusTag uppercaseString] containsString:@"CONTINUE"]);
    BOOL anyAction = (ranLinux || ranMacos || ranWeb);

    if (anyAction) {
        scan_directory([sharedDir UTF8String]);
        NSString *summary = [actionsDone componentsJoinedByString:@"+"];
        snprintf(g_ai_last_action, sizeof(g_ai_last_action), "STEP %d/%d EXEC: %s", step, g_al_max_steps, [summary UTF8String]);
        snprintf(g_status, sizeof(g_status), "AL STEP %d/%d -> EXECUTING %s & OBSERVING RESULTS...", step, g_al_max_steps, [summary UTF8String]);
    } else {
        g_al_step = 0;
        snprintf(g_ai_last_action, sizeof(g_ai_last_action), "AL COMPLETE (Step %d/%d)", step, g_al_max_steps);
        snprintf(g_status, sizeof(g_status), "AL LOOP COMPLETE (STEP %d/%d)", step, g_al_max_steps);
        play_pcm_chime();
        return;
    }

    double delaySec = ranWeb ? 3.2 : 2.2;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delaySec * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        capture_web_dom_text(^{
            pthread_mutex_lock(&g_lock);
            scan_directory([sharedDir UTF8String]);
            if (g_al_abort || !g_ai_auto_exec) {
                g_al_step = 0;
                snprintf(g_ai_last_action, sizeof(g_ai_last_action), "AL LOOP STOPPED");
                pthread_mutex_unlock(&g_lock);
                return;
            }

            BOOL hasError = NO;
            NSString *obs = harvest_execution_logs(&hasError);
            if ((hasError || wantContinue) && step < g_al_max_steps) {
                int nextStep = step + 1;
                g_al_step = nextStep;
                const char *reason = hasError ? "SELF-HEALING ERROR..." : "READING WEB/OUTPUT FOR NEXT STEP...";
                snprintf(g_ai_last_action, sizeof(g_ai_last_action), "STEP %d/%d: %s", nextStep, g_al_max_steps, reason);
                snprintf(g_status, sizeof(g_status), "AL LOOP STEP %d/%d -> %s", nextStep, g_al_max_steps, reason);
                NSString *obsCopy = obs.length > 0 ? obs : @"Web navigation completed; see Live WebKit Viewport text above.";
                pthread_mutex_unlock(&g_lock);
                run_al_step(nextStep, [obsCopy UTF8String]);
            } else {
                g_al_step = 0;
                snprintf(g_ai_last_action, sizeof(g_ai_last_action), hasError ? "AL FINISHED WITH WARNING (Step %d)" : "AL COMPLETE & VERIFIED (Step %d/%d)", step, g_al_max_steps);
                snprintf(g_status, sizeof(g_status), "AL CLOSED-LOOP VERIFIED (STEP %d/%d)", step, g_al_max_steps);
                play_pcm_chime();
                pthread_mutex_unlock(&g_lock);
            }
        });
    });
}

static void run_al_step(int step, const char *observation) {
    pthread_mutex_lock(&g_lock);
    NSString *goalStr   = [NSString stringWithUTF8String:g_al_goal] ?: @"";
    NSString *obsStr    = [NSString stringWithUTF8String:observation ?: ""] ?: @"";
    NSString *webUrl    = [NSString stringWithUTF8String:g_browser_url] ?: @"";
    NSString *webTitle  = [NSString stringWithUTF8String:g_web_title_cache] ?: @"";
    NSString *webText   = [NSString stringWithUTF8String:g_web_text_cache] ?: @"";
    NSString *modelName = [NSString stringWithUTF8String:g_ai_model] ?: @"llama3.2";
    pthread_mutex_unlock(&g_lock);

    NSURL *url = [NSURL URLWithString:@"http://127.0.0.1:11434/api/generate"];
    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:url];
    req.HTTPMethod = @"POST";
    req.timeoutInterval = 45.0;
    [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];

    NSString *sysPrompt =
        @"You are the Closed-Loop AL (Autonomous Layer) Agent inside ZERO OS (macOS host + ARM64 Alpine Linux guest sharing /mnt/macos <-> ~/zero_os/shared + live WebKit browser).\n"
        @"You operate in a multi-step perception-action loop (max 3 steps). You can see the LIVE WebKit browser text and previous command outputs/exit codes.\n"
        @"Respond concisely in the user's language, and output executable action blocks using these exact tags:\n"
        @"[LINUX]\n<POSIX /bin/sh commands for Alpine Linux; shared dir is /mnt/macos>\n[/LINUX]\n"
        @"[MACOS]\n<zsh commands for macOS host; shared dir is ~/zero_os/shared>\n[/MACOS]\n"
        @"[WEB]\n<plain search keywords like Apple Silicon, or exact https:// URL>\n[/WEB]\n"
        @"[SPACE]\n<LINUX or MACOS or WEB or FILES>\n[/SPACE]\n"
        @"[STATUS]\n<CONTINUE if you need to read the result of this step's [WEB] or [LINUX] in the next step, otherwise DONE>\n[/STATUS]\n"
        @"Critical Rules:\n"
        @"1. In [LINUX], use POSIX /bin/sh only (use seq 1 9, NEVER {0..9} braces) and write files to /mnt/macos/ (NEVER /mnt/macos/shared/).\n"
        @"2. In [MACOS], NEVER access /mnt/macos; read ~/zero_os/shared/ directly.\n"
        @"3. In [WEB], for searches output ONLY plain search keywords (e.g. Apple Silicon) and NEVER guess/invent URLs.\n"
        @"4. If the user asks to read/summarize the currently open Web page, use the Live WebKit Content provided below immediately without re-navigating.\n"
        @"5. If the user asks to search the Web first and then save/process what you find, output [WEB]keywords[/WEB] and [STATUS]CONTINUE[/STATUS] on Step 1, then on Step 2 read the updated WebKit Content and write the file.\n"
        @"6. If Previous Execution Observation shows a non-zero EXIT code or error, fix the command and re-run it.";

    NSString *fullPrompt = [NSString stringWithFormat:
        @"%@\n\n=== CURRENT PERCEPTION (STEP %d / %d) ===\n"
        @"User Goal: %@\n"
        @"Live WebKit Browser URL: %@\n"
        @"Live WebKit Page Title: %@\n"
        @"Live WebKit Page Text: %@\n"
        @"Previous Execution Observation:\n%@\n",
        sysPrompt, step, g_al_max_steps, goalStr, webUrl, webTitle,
        webText.length > 0 ? webText : @"(empty)", obsStr];

    NSDictionary *body = @{
        @"model": modelName,
        @"prompt": fullPrompt,
        @"stream": @NO,
        @"keep_alive": @"30m"
    };
    req.HTTPBody = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];

    [[[NSURLSession sharedSession] dataTaskWithRequest:req completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
        pthread_mutex_lock(&g_lock);
        if (g_al_abort) {
            g_al_step = 0;
            pthread_mutex_unlock(&g_lock);
            return;
        }
        if (err || !data) {
            g_al_step = 0;
            snprintf(g_ai_reply, sizeof(g_ai_reply), "Ollama waking up -> auto-launching Ollama.app...");
            snprintf(g_ai_last_action, sizeof(g_ai_last_action), "WAKING OLLAMA...");
            system("open -a Ollama >/dev/null 2>&1 &");
        } else {
            NSDictionary *json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
            NSString *res = json[@"response"];
            if (res) {
                NSString *clean = [res stringByReplacingOccurrencesOfString:@"\r" withString:@""];
                snprintf(g_ai_reply, sizeof(g_ai_reply), "[AL STEP %d/%d]\n%s", step, g_al_max_steps, [clean UTF8String]);
                dispatch_al_step_response(step, clean);
            } else {
                g_al_step = 0;
            }
        }
        g_ai_scroll = 0;
        pthread_mutex_unlock(&g_lock);
    }] resume];
}

static void start_autonomous_loop(const char *prompt) {
    if (!prompt || !prompt[0]) return;
    pthread_mutex_lock(&g_lock);
    strncpy(g_al_goal, prompt, sizeof(g_al_goal) - 1);
    g_al_goal[sizeof(g_al_goal) - 1] = '\0';
    g_al_abort = 0;
    g_al_step = 1;
    snprintf(g_ai_reply, sizeof(g_ai_reply), "[AL STEP 1/%d] Perceiving WebKit DOM & spatial state...", g_al_max_steps);
    snprintf(g_ai_last_action, sizeof(g_ai_last_action), "AL STEP 1/%d: PERCEIVING...", g_al_max_steps);
    g_ai_scroll = 0;
    pthread_mutex_unlock(&g_lock);

    save_atom_snapshot_file("zero_auto.atom", YES);
    capture_web_dom_text(^{
        run_al_step(1, "Initial step: no commands executed yet.");
    });
}

// --- .atom Binary State Crystallization ---
static void save_atom_snapshot_file(const char *filename, BOOL silent) {
    ZeroAtomSnapshot *snap = calloc(1, sizeof(ZeroAtomSnapshot));
    snap->magic = 0x5A455244;
    memcpy(snap->wins, g_wins, sizeof(g_wins));
    memcpy(snap->z_order, g_z_order, sizeof(g_z_order));
    strncpy(snap->cwd, g_cwd, sizeof(snap->cwd) - 1);
    strncpy(snap->browser_url, g_browser_url, sizeof(snap->browser_url) - 1);
    strncpy(snap->status_msg, g_status, sizeof(snap->status_msg) - 1);
    strncpy(snap->ai_reply, g_ai_reply, sizeof(snap->ai_reply) - 1);
    snap->img_w = g_web_w;
    snap->img_h = g_web_h;
    memcpy(snap->img_buf, g_web_img, sizeof(g_web_img));

    NSString *rel = [NSString stringWithFormat:@"zero_os/%s", filename];
    NSString *path = [NSHomeDirectory() stringByAppendingPathComponent:rel];
    FILE *f = fopen([path UTF8String], "wb");
    if (f) {
        fwrite(snap, sizeof(ZeroAtomSnapshot), 1, f);
        fclose(f);
        if (!silent) {
            snprintf(g_status, sizeof(g_status), "CRYSTALLIZED 5-WINDOW STATE -> %s (%lu KB)", filename, sizeof(ZeroAtomSnapshot) / 1024);
            play_pcm_chime();
        }
    }
    free(snap);
}

static void restore_atom_snapshot_file(const char *filename) {
    NSString *rel = [NSString stringWithFormat:@"zero_os/%s", filename];
    NSString *path = [NSHomeDirectory() stringByAppendingPathComponent:rel];
    FILE *f = fopen([path UTF8String], "rb");
    if (!f) {
        snprintf(g_status, sizeof(g_status), "NO %s FOUND (PRESS [F6] FIRST)", filename);
        return;
    }
    ZeroAtomSnapshot *snap = calloc(1, sizeof(ZeroAtomSnapshot));
    if (fread(snap, sizeof(ZeroAtomSnapshot), 1, f) == 1 && snap->magic == 0x5A455244) {
        g_al_abort = 1;
        g_al_step = 0;
        memcpy(g_wins, snap->wins, sizeof(g_wins));
        memcpy(g_z_order, snap->z_order, sizeof(g_z_order));
        scan_directory(snap->cwd);
        navigate_browser_to(snap->browser_url);
        strncpy(g_ai_reply, snap->ai_reply, sizeof(g_ai_reply) - 1);
        g_ai_scroll = 0;
        g_web_w = snap->img_w;
        g_web_h = snap->img_h;
        memcpy(g_web_img, snap->img_buf, sizeof(g_web_img));
        snprintf(g_status, sizeof(g_status), "RESTORED SPATIAL CHECKPOINT (%s) IN <1 MS", filename);
        play_pcm_chime();
    } else {
        snprintf(g_status, sizeof(g_status), "PREVIOUS .ATOM FORMAT — PRESS [F6] TO SAVE v0.7.0 STATE");
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
    NSString *ns = [NSString stringWithUTF8String:text];
    if (!ns) return 1;

    for (NSUInteger i = 0; i < ns.length && count < AI_MAX_LINES; i++) {
        unichar ch = [ns characterAtIndex:i];
        if (ch == '\n') {
            count++;
            col = 0;
        } else if (ch >= 32) {
            int step = (ch > 0x7F) ? 2 : 1;
            if (col + step > cols) {
                count++;
                col = 0;
                if (count >= AI_MAX_LINES) break;
            }
            NSString *sub = [ns substringWithRange:NSMakeRange(i, 1)];
            const char *u = [sub UTF8String];
            if (u) {
                size_t ulen = strlen(u);
                size_t curByte = strlen(lines[count]);
                if (curByte + ulen < AI_MAX_COLS) {
                    strcat(lines[count], u);
                    col += step;
                }
            }
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

    int cbx = w->x + w->w - 64;
    int cby = w->y + 5;
    fill_rect(cbx, cby, 54, 18, 0xFF0F172A);
    draw_border(cbx, cby, 54, 18, w->border_col, 1);
    draw_text(cbx + 7, cby + 2, "[COPY]", 10.5, accent, YES);

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
    int max_path_chars = MAX(14, (w->w - 24) / 7);
    snprintf(cwd_disp, sizeof(cwd_disp), "PATH: %.*s", max_path_chars, g_cwd);
    draw_text(w->x + 10, w->y + 34, cwd_disp, 10.5, cDim, NO);

    int list_y = w->y + 52;
    int list_h = MAX(40, w->h - 64);
    int max_vis = MAX(2, list_h / 19);
    int start_idx = (g_selected_file >= max_vis) ? (g_selected_file - max_vis + 1) : 0;
    int name_cols = MAX(10, (w->w - 118) / 7);

    for (int i = 0; i < max_vis && (start_idx + i) < g_file_count; i++) {
        int idx = start_idx + i;
        int ry = list_y + i * 19;
        if (idx == g_selected_file) fill_rect(w->x + 6, ry - 2, w->w - 14, 18, 0xFF26334D);
        char line[160];
        if (g_files[idx].is_dir) {
            snprintf(line, sizeof(line), "[D] %-*.*s <DIR>", name_cols, name_cols, g_files[idx].name);
            draw_text(w->x + 10, ry, line, 11, cAmber, idx == g_selected_file);
        } else {
            snprintf(line, sizeof(line), "[F] %-*.*s %6lldB", name_cols, name_cols, g_files[idx].name, g_files[idx].size);
            draw_text(w->x + 10, ry, line, 11, cWhite, idx == g_selected_file);
        }
    }
}

static void render_ai_hud_window(SpatialWindow *w, NSColor *cCyan, NSColor *cGreen, NSColor *cAmber, NSColor *cWhite, NSColor *cDim) {
    char info1[110], info2[110];
    snprintf(info1, sizeof(info1), "RAM: %.1fMB | %s | AUTO:%s",
             get_mach_ram_mb(), g_ai_model, g_ai_auto_exec ? "ON" : "OFF");
    snprintf(info2, sizeof(info2), "%s", g_ai_last_action);
    draw_text(w->x + 12, w->y + 34, info1, 10.5, cGreen, YES);
    draw_text(w->x + 12, w->y + 52, info2, 10.5, g_al_step > 0 ? cCyan : cAmber, YES);

    if (g_web_w > 0 && g_web_h > 0 && w->w >= 400) {
        int tw = 84, th = 56;
        int tx = w->x + w->w - tw - 12;
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

    int bx = w->x + 12;
    int by = w->y + 98;
    int bw = MAX(180, w->w - 24);
    int bh = MAX(80, w->h - 114);
    fill_rect(bx, by, bw, bh, 0xFF080A10);
    draw_border(bx, by, bw, bh, g_ai_input_mode ? 0xFFF59E0B : (g_al_step > 0 ? 0xFF38BDF8 : 0xFF3B4254), 1);
    draw_text(bx + 8, by + 6, "AL LOOP — [F8] Goal | [F12] Rollback | [Esc] Stop", 10.5, cCyan, YES);

    if (g_ai_input_mode) {
        NSString *pNs = [NSString stringWithUTF8String:g_ai_prompt];
        NSString *disp = pNs ?: @"";
        if (disp.length > 36) disp = [disp substringFromIndex:disp.length - 36];
        char pbuf[256];
        snprintf(pbuf, sizeof(pbuf), "GOAL> %s_", [disp UTF8String]);
        draw_text(bx + 8, by + 30, pbuf, 11, cAmber, YES);
        draw_text(bx + 8, by + bh - 20, "[Cmd+V] Paste JP/EN | [Enter] Start AL | [Esc] Cancel", 10, cDim, NO);
    } else {
        int cols = MAX(16, (bw - 30) / 7);
        int vis_rows = MAX(2, (bh - 44) / 16);
        char lines[AI_MAX_LINES][AI_MAX_COLS + 1];
        g_ai_total_lines = wrap_ai_lines(g_ai_reply, cols, lines);
        int max_scroll = MAX(0, g_ai_total_lines - vis_rows);
        if (g_ai_scroll > max_scroll) g_ai_scroll = max_scroll;
        if (g_ai_scroll < 0) g_ai_scroll = 0;

        for (int r = 0; r < vis_rows; r++) {
            int lidx = g_ai_scroll + r;
            if (lidx < g_ai_total_lines && lines[lidx][0]) {
                draw_text(bx + 8, by + 25 + r * 16, lines[lidx], 11, cWhite, NO);
            }
        }

        int tx = bx + bw - 12;
        int ty = by + 24;
        int tw = 7;
        int th = MAX(16, bh - 42);
        fill_rect(tx, ty, tw, th, 0xFF1E293B);
        int thumb_h = (g_ai_total_lines <= vis_rows) ? th : MAX(12, (th * vis_rows) / g_ai_total_lines);
        int thumb_y = ty;
        if (max_scroll > 0) {
            thumb_y = ty + ((th - thumb_h) * g_ai_scroll) / max_scroll;
        }
        fill_rect(tx, thumb_y, tw, thumb_h, 0xFFA855F7);

        char hint[80];
        snprintf(hint, sizeof(hint), "[Cmd+C] Copy | Line %d/%d",
                 MIN(g_ai_scroll + vis_rows, g_ai_total_lines), g_ai_total_lines);
        draw_text(bx + 8, by + bh - 17, hint, 10, cDim, NO);
    }
}

static void render_browser_window(SpatialWindow *w, BOOL is_front, NSColor *cPink, NSColor *cAmber, NSColor *cWhite, NSColor *cDim) {
    int bar_y = w->y + 32;
    fill_rect(w->x + 10, bar_y, 26, 20, 0xFF1E293B);
    draw_border(w->x + 10, bar_y, 26, 20, 0xFFEC4899, 1);
    draw_text(w->x + 16, bar_y + 3, "<", 11, cWhite, YES);

    fill_rect(w->x + 40, bar_y, 26, 20, 0xFF1E293B);
    draw_border(w->x + 40, bar_y, 26, 20, 0xFFEC4899, 1);
    draw_text(w->x + 48, bar_y + 3, "R", 11, cWhite, YES);

    int ux = w->x + 72;
    int uw = MAX(100, w->w - 84);
    fill_rect(ux, bar_y, uw, 20, 0xFF07090F);
    draw_border(ux, bar_y, uw, 20, g_url_input_mode ? 0xFFF59E0B : 0xFF475569, 1);

    int max_url_chars = MAX(14, (uw - 12) / 7);
    if (g_url_input_mode) {
        size_t len = strlen(g_url_input);
        const char *tail = (len > (size_t)max_url_chars) ? (g_url_input + len - max_url_chars) : g_url_input;
        char udisp[256];
        snprintf(udisp, sizeof(udisp), "%s_", tail);
        draw_text(ux + 6, bar_y + 3, udisp, 11, cAmber, YES);
    } else {
        char udisp[256];
        snprintf(udisp, sizeof(udisp), "%.*s", max_url_chars, g_browser_url);
        draw_text(ux + 6, bar_y + 3, udisp, 11, cWhite, NO);
    }

    int vx = w->x + 4;
    int vy = w->y + 56;
    int vw = w->w - 8;
    int vh = w->h - 74;

    if (g_browser_snap && g_snap_w > 0 && g_snap_h > 0 && vw > 0 && vh > 0) {
        for (int j = 0; j < vh; j++) {
            int dy = vy + j;
            if (dy < 0 || dy >= FB_H) continue;
            int sy = (j * g_snap_h) / vh;
            for (int i = 0; i < vw; i++) {
                int dx = vx + i;
                if (dx < 0 || dx >= FB_W) continue;
                int sx = (i * g_snap_w) / vw;
                g_fb[dy * FB_W + dx] = g_browser_snap[sy * g_snap_w + sx];
            }
        }
    } else {
        fill_rect(vx, vy, vw, vh, 0xFF141926);
        draw_text(vx + 16, vy + 20, "Loading Native WebKit Viewport...", 12, cDim, NO);
    }

    draw_text(w->x + 12, w->y + w->h - 16, "[F2] URL | Live DOM Text Streamed Directly to AL Perception", 10, cDim, NO);
}

static void render_spatial_window(SpatialWindow *w, BOOL is_front) {
    NSColor *cCyan  = [NSColor colorWithRed:0.25 green:0.90 blue:0.95 alpha:1.0];
    NSColor *cGreen = [NSColor colorWithRed:0.30 green:0.95 blue:0.55 alpha:1.0];
    NSColor *cAmber = [NSColor colorWithRed:1.00 green:0.78 blue:0.25 alpha:1.0];
    NSColor *cPink  = [NSColor colorWithRed:0.95 green:0.35 blue:0.65 alpha:1.0];
    NSColor *cWhite = [NSColor colorWithWhite:0.92 alpha:1.0];
    NSColor *cDim   = [NSColor colorWithWhite:0.58 alpha:1.0];

    fill_rect(w->x + 6, w->y + 6, w->w, w->h, 0xFF04060A);
    fill_rect(w->x, w->y, w->w, w->h, w->body_bg);
    fill_rect(w->x, w->y, w->w, 28, is_front ? w->title_bg : 0xFF161B26);
    draw_border(w->x, w->y, w->w, w->h, is_front ? w->border_col : 0xFF334155, is_front ? 2 : 1);

    CGContextSaveGState(g_ctx);
    CGContextClipToRect(g_ctx, CGRectMake(w->x + 2, w->y + 2, w->w - 4, w->h - 4));

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
    } else if (w->id == WIN_BROWSER) {
        render_browser_window(w, is_front, cPink, cAmber, cWhite, cDim);
    }

    int gx = w->x + w->w - 14;
    int gy = w->y + w->h - 14;
    for (int k = 0; k < 10; k += 3) {
        fill_rect(gx + k, gy + 10 - k, 10 - k, 2, is_front ? w->border_col : 0xFF475569);
    }

    CGContextRestoreGState(g_ctx);
}

static void render_frame(void) {
    pthread_mutex_lock(&g_lock);

    fill_rect(0, 0, FB_W, FB_H, 0xFF070A10);
    for (int gy = 24; gy < FB_H - 24; gy += 32) {
        for (int gx = 24; gx < FB_W; gx += 32) {
            g_fb[gy * FB_W + gx] = 0xFF161E2E;
        }
    }

    NSGraphicsContext *prevCtx = [NSGraphicsContext currentContext];
    NSGraphicsContext *nsCtx = [NSGraphicsContext graphicsContextWithCGContext:g_ctx flipped:YES];
    [NSGraphicsContext setCurrentContext:nsCtx];

    for (int i = 0; i < WIN_COUNT; i++) {
        int wid = g_z_order[i];
        BOOL is_front = (i == WIN_COUNT - 1);
        render_spatial_window(&g_wins[wid], is_front);
    }

    NSColor *cGreen = [NSColor colorWithRed:0.30 green:0.95 blue:0.55 alpha:1.0];
    NSColor *cCyan  = [NSColor colorWithRed:0.25 green:0.90 blue:0.95 alpha:1.0];
    fill_rect(0, FB_H - 24, FB_W, 24, 0xFF0F172A);
    fill_rect(0, FB_H - 24, FB_W, 1, 0xFF1E293B);
    draw_text(14, FB_H - 20, g_status, 11, cGreen, YES);
    draw_text(700, FB_H - 20, "[F1] Win | [F3] Shared | [F8] AL Goal | [F11] Auto | [F12] Rollback", 10.5, cCyan, NO);

    [NSGraphicsContext setCurrentContext:prevCtx];
    pthread_mutex_unlock(&g_lock);
}

@interface ZeroView : NSView <WKNavigationDelegate> {
    int _dragMode;
    int _dragWinId;
    int _dragOffX, _dragOffY;
    int _origW, _origH;
}
@end

@implementation ZeroView
- (BOOL)acceptsFirstResponder { return YES; }

- (void)captureBrowserSnapshot {
    if (!g_webView || g_webView.isHidden) return;
    WKSnapshotConfiguration *scfg = [[WKSnapshotConfiguration alloc] init];
    [g_webView takeSnapshotWithConfiguration:scfg completionHandler:^(NSImage *img, NSError *err) {
        if (err || !img) return;
        CGImageRef cgImg = [img CGImageForProposedRect:NULL context:nil hints:nil];
        if (!cgImg) return;
        int w = (int)CGImageGetWidth(cgImg);
        int h = (int)CGImageGetHeight(cgImg);
        if (w <= 0 || h <= 0) return;

        uint32_t *buf = malloc((size_t)w * (size_t)h * sizeof(uint32_t));
        if (!buf) return;
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ic = CGBitmapContextCreate(buf, w, h, 8, w * 4, cs,
                                                kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);
        CGContextDrawImage(ic, CGRectMake(0, 0, w, h), cgImg);
        CGContextRelease(ic);
        CGColorSpaceRelease(cs);

        pthread_mutex_lock(&g_lock);
        free(g_browser_snap);
        g_browser_snap = buf;
        g_snap_w = w;
        g_snap_h = h;
        pthread_mutex_unlock(&g_lock);
    }];
}

- (void)syncWebViewOverlay {
    if (!g_webView) return;
    pthread_mutex_lock(&g_lock);
    SpatialWindow bw = g_wins[WIN_BROWSER];
    BOOL occluded = is_browser_occluded();
    BOOL dragging = (_dragMode == 1 || _dragMode == 2);
    if (g_webView.URL) {
        const char *cur = [[g_webView.URL absoluteString] UTF8String];
        if (cur && cur[0]) strncpy(g_browser_url, cur, sizeof(g_browser_url) - 1);
    }
    pthread_mutex_unlock(&g_lock);

    if (occluded || dragging) {
        if (!g_webView.isHidden) [g_webView setHidden:YES];
        return;
    }

    CGFloat scaleX = self.bounds.size.width / (CGFloat)FB_W;
    CGFloat scaleY = self.bounds.size.height / (CGFloat)FB_H;
    CGFloat vx = (bw.x + 4) * scaleX;
    CGFloat vy = (FB_H - (bw.y + bw.h - 18)) * scaleY;
    CGFloat vw = MAX(40, (bw.w - 8) * scaleX);
    CGFloat vh = MAX(40, (bw.h - 74) * scaleY);

    NSRect targetFrame = NSMakeRect(vx, vy, vw, vh);
    if (!NSEqualRects(g_webView.frame, targetFrame)) {
        [g_webView setFrame:targetFrame];
    }
    if (g_webView.isHidden) [g_webView setHidden:NO];

    g_snap_tick++;
    if (g_snap_tick % 30 == 0) {
        [self captureBrowserSnapshot];
    }
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    [self captureBrowserSnapshot];
    capture_web_dom_text(nil);
    pthread_mutex_lock(&g_lock);
    if (g_al_step == 0) {
        snprintf(g_status, sizeof(g_status), "WEB STREAM & DOM PERCEPTION READY: %s", g_browser_url);
    }
    pthread_mutex_unlock(&g_lock);
}

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
    int by = w->y + 98;
    int bh = MAX(80, w->h - 114);
    int vis_rows = MAX(2, (bh - 44) / 16);
    int max_scroll = MAX(0, g_ai_total_lines - vis_rows);
    int ty = by + 24;
    int th = MAX(16, bh - 42);
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
    [self.window makeFirstResponder:self];
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

        if ((wid == WIN_LINUX || wid == WIN_MACOS) &&
            fx >= w->x + w->w - 66 && fx <= w->x + w->w - 8 &&
            fy >= w->y + 4 && fy <= w->y + 24) {
            [self copyConsoleLog:wid];
            pthread_mutex_unlock(&g_lock);
            return;
        }

        if (fy <= w->y + 28) {
            [self captureBrowserSnapshot];
            _dragMode = 1;
            _dragOffX = fx - w->x;
            _dragOffY = fy - w->y;
            g_ai_input_mode = 0;
            g_url_input_mode = 0;
            pthread_mutex_unlock(&g_lock);
            return;
        }

        if (fx >= w->x + w->w - 20 && fy >= w->y + w->h - 20) {
            [self captureBrowserSnapshot];
            _dragMode = 2;
            _dragOffX = fx;
            _dragOffY = fy;
            _origW = w->w;
            _origH = w->h;
            pthread_mutex_unlock(&g_lock);
            return;
        }

        if (wid == WIN_BROWSER) {
            g_ai_input_mode = 0;
            int bar_y = w->y + 32;
            if (fy >= bar_y && fy <= bar_y + 22) {
                if (fx >= w->x + 10 && fx <= w->x + 36) {
                    dispatch_async(dispatch_get_main_queue(), ^{ [g_webView goBack]; });
                } else if (fx >= w->x + 40 && fx <= w->x + 66) {
                    dispatch_async(dispatch_get_main_queue(), ^{ [g_webView reload]; });
                } else if (fx >= w->x + 72) {
                    g_url_input_mode = 1;
                    g_url_input[0] = '\0';
                    snprintf(g_status, sizeof(g_status), "URL INPUT MODE: PRESS [CMD+V] TO PASTE URL OR TYPE & PRESS [ENTER]");
                }
            } else {
                g_url_input_mode = 0;
            }
            pthread_mutex_unlock(&g_lock);
            return;
        }

        g_url_input_mode = 0;

        if ((wid == WIN_LINUX || wid == WIN_MACOS) && fx >= w->x + w->w - 20) {
            _dragMode = 4;
            [self updateTermScroll:wid fromY:fy];
            pthread_mutex_unlock(&g_lock);
            return;
        }

        if (wid == WIN_AI_HUD) {
            int by = w->y + 98;
            if (fy >= by) {
                if (fx >= w->x + w->w - 32) {
                    _dragMode = 3;
                    [self updateAiScrollFromY:fy];
                } else {
                    g_ai_input_mode = 1;
                    g_ai_prompt[0] = '\0';
                    snprintf(g_status, sizeof(g_status), "AL GOAL MODE: [CMD+V] PASTE JP/EN GOAL & PRESS [ENTER]");
                }
            }
        } else {
            g_ai_input_mode = 0;
            if (wid == WIN_FILES && fy >= w->y + 52) {
                int clicked = (fy - (w->y + 52)) / 19;
                int max_vis = MAX(2, (w->h - 64) / 19);
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
    BOOL wasBrowserResize = (_dragMode == 2 && _dragWinId == WIN_BROWSER);
    _dragMode = 0;
    _dragWinId = -1;
    if (wasBrowserResize) {
        [self syncWebViewOverlay];
        [self captureBrowserSnapshot];
    }
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
        int bh = MAX(80, w->h - 114);
        int vis_rows = MAX(2, (bh - 44) / 16);
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
    if (g_url_input_mode) {
        NSString *flat = [[clip stringByReplacingOccurrencesOfString:@"\r" withString:@""]
                                stringByReplacingOccurrencesOfString:@"\n" withString:@""];
        const char *u = [flat UTF8String];
        if (u) {
            size_t cur = strlen(g_url_input);
            size_t rem = sizeof(g_url_input) - 1 - cur;
            if (rem > 0) strncat(g_url_input, u, rem);
            snprintf(g_status, sizeof(g_status), "PASTED URL — PRESS [ENTER] TO NAVIGATE");
        }
    } else if (g_ai_input_mode) {
        NSString *flat = [[clip stringByReplacingOccurrencesOfString:@"\r" withString:@" "]
                                stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
        const char *u = [flat UTF8String];
        if (u) {
            size_t cur = strlen(g_ai_prompt);
            size_t rem = sizeof(g_ai_prompt) - 1 - cur;
            if (rem > 0) strncat(g_ai_prompt, u, rem);
            snprintf(g_status, sizeof(g_status), "PASTED AL GOAL (%lu BYTES) — PRESS [ENTER] TO LAUNCH", strlen(g_ai_prompt));
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
        snprintf(g_status, sizeof(g_status), "COPIED AL LOG TO CLIPBOARD (%lu CHARS)", (unsigned long)text.length);
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

    if (kc == 53 && g_al_step > 0) {
        pthread_mutex_lock(&g_lock);
        g_al_abort = 1;
        g_al_step = 0;
        snprintf(g_ai_last_action, sizeof(g_ai_last_action), "AL LOOP ABORTED BY USER");
        snprintf(g_status, sizeof(g_status), "ABORTED CLOSED-LOOP AL EXECUTION");
        pthread_mutex_unlock(&g_lock);
        return;
    }

    if (kc == 122) { // F1
        pthread_mutex_lock(&g_lock);
        int back_id = g_z_order[0];
        bring_window_to_front(back_id);
        g_ai_input_mode = 0;
        g_url_input_mode = 0;
        snprintf(g_status, sizeof(g_status), "ACTIVE WINDOW: %s", g_wins[back_id].title);
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (kc == 120 || (kc == 53 && g_url_input_mode)) { // F2 or Esc
        pthread_mutex_lock(&g_lock);
        g_url_input_mode = !g_url_input_mode;
        g_ai_input_mode = 0;
        g_url_input[0] = '\0';
        if (g_url_input_mode) bring_window_to_front(WIN_BROWSER);
        snprintf(g_status, sizeof(g_status), g_url_input_mode ? "URL INPUT MODE: TYPE OR [CMD+V] PASTE URL & PRESS [ENTER]" : "EXITED URL INPUT MODE");
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (kc == 99) { // F3
        char shared[512];
        snprintf(shared, sizeof(shared), "%s/zero_os/shared", getenv("HOME"));
        scan_directory(shared);
        bring_window_to_front(WIN_FILES);
        snprintf(g_status, sizeof(g_status), "VIRTIOFS SHARED DIR: ~/zero_os/shared <-> /mnt/macos");
        return;
    }
    if (kc == 118) { boot_native_linux_kernel(); return; }                  // F4
    if (kc == 96)  { fetch_live_web_image(); return; }                      // F5
    if (kc == 97)  { save_atom_snapshot_file("state.atom", NO); return; }   // F6
    if (kc == 98)  { restore_atom_snapshot_file("state.atom"); return; }    // F7
    if (kc == 100 || (kc == 53 && g_ai_input_mode)) {                       // F8 or Esc
        g_ai_input_mode = !g_ai_input_mode;
        g_url_input_mode = 0;
        g_ai_prompt[0] = '\0';
        if (g_ai_input_mode) bring_window_to_front(WIN_AI_HUD);
        snprintf(g_status, sizeof(g_status), g_ai_input_mode ? "AL GOAL MODE: [CMD+V] PASTE JP/EN GOAL & PRESS [ENTER]" : "EXITED AL GOAL MODE");
        return;
    }
    if (kc == 101) { play_pcm_chime(); return; }                            // F9
    if (kc == 109) {                                                        // F10
        pthread_mutex_lock(&g_lock);
        reset_spatial_layout();
        snprintf(g_status, sizeof(g_status), "RESET 5 SPATIAL WINDOWS TO DEFAULT MULTI-OS LAYOUT");
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (kc == 103) {                                                        // F11
        pthread_mutex_lock(&g_lock);
        g_ai_auto_exec = !g_ai_auto_exec;
        snprintf(g_status, sizeof(g_status), "AL AUTO-EXECUTION: %s", g_ai_auto_exec ? "ENABLED (CLOSED-LOOP)" : "DISABLED (PLAN ONLY)");
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (kc == 111) {                                                        // F12
        pthread_mutex_lock(&g_lock);
        restore_atom_snapshot_file("zero_auto.atom");
        pthread_mutex_unlock(&g_lock);
        return;
    }

    if (g_url_input_mode) {
        if (kc == 36) {
            navigate_browser_to(g_url_input);
            g_url_input_mode = 0;
        } else if (kc == 51) {
            size_t len = strlen(g_url_input);
            if (len > 0) g_url_input[len - 1] = '\0';
        } else if (chars.length > 0) {
            const char *u = [chars UTF8String];
            if (u && strlen(g_url_input) + strlen(u) < sizeof(g_url_input) - 1) {
                strcat(g_url_input, u);
            }
        }
        return;
    }

    if (g_ai_input_mode) {
        if (kc == 36) {
            start_autonomous_loop(g_ai_prompt);
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
    if (kc == 31 && front_window_id() == WIN_FILES) {
        if (g_selected_file < g_file_count && g_files[g_selected_file].is_dir) {
            char next[1024];
            snprintf(next, sizeof(next), "%s/%s", g_cwd, g_files[g_selected_file].name);
            char resolved[512];
            if (realpath(next, resolved)) scan_directory(resolved);
        }
        return;
    }

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
        detect_ollama_model();
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
        [win setTitle:@"ZERO OS v0.7.0 — Closed-Loop AL Agent & WebKit DOM Perception"];
        ZeroView *view = [[ZeroView alloc] initWithFrame:NSMakeRect(0, 0, winW, winH)];
        [win setContentView:view];

        WKWebViewConfiguration *wcfg = [[WKWebViewConfiguration alloc] init];
        g_webView = [[WKWebView alloc] initWithFrame:NSMakeRect(600, 380, 650, 320) configuration:wcfg];
        g_webView.navigationDelegate = view;
        [view addSubview:g_webView];
        navigate_browser_to(g_browser_url);

        [win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        [NSTimer scheduledTimerWithTimeInterval:(1.0 / 30.0) repeats:YES block:^(NSTimer *t) {
            [view syncWebViewOverlay];
            [view setNeedsDisplay:YES];
        }];

        [NSApp run];
    }
    return 0;
}
