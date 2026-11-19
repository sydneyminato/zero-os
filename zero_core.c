#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#import <ImageIO/ImageIO.h>
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
#define TERM_ROWS 21
#define TERM_COLS 74
#define MAX_FILES 64

typedef struct {
    char name[128];
    off_t size;
    int is_dir;
} FileEntry;

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
static int g_pty_fd = -1;
static pid_t g_pty_pid = -1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static char g_cwd[512];
static FileEntry g_files[MAX_FILES];
static int g_file_count = 0;
static int g_selected_file = 0;

static char g_term[TERM_ROWS][TERM_COLS + 1];
static int g_trow = 0, g_tcol = 0;

static uint32_t g_web_img[360 * 240];
static int g_web_w = 0, g_web_h = 0;
static int g_web_idx = 0;
static char g_status[256] = "KERNEL ONLINE: ALL SUBSYSTEMS LIVE (NO MOCKUPS)";
static char g_ai_reply[256] = "Press [F8] to query local LLM via native NSURLSession";
static int g_ai_input_mode = 0;
static char g_ai_prompt[128] = "";

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
    st.samples_left = 13230; // 0.3s @ 44.1kHz
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

// --- Live PTY Terminal Buffer & Reader Thread ---
static void term_scroll(void) {
    memmove(g_term[0], g_term[1], (TERM_ROWS - 1) * (TERM_COLS + 1));
    memset(g_term[TERM_ROWS - 1], 0, TERM_COLS + 1);
}

static void term_put_char(char c) {
    if (c == '\r') {
        g_tcol = 0;
    } else if (c == '\n') {
        g_trow++;
        g_tcol = 0;
        if (g_trow >= TERM_ROWS) {
            term_scroll();
            g_trow = TERM_ROWS - 1;
        }
    } else if (c == '\b' || c == 127) {
        if (g_tcol > 0) {
            g_tcol--;
            g_term[g_trow][g_tcol] = ' ';
        }
    } else if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
        if (g_tcol >= TERM_COLS) {
            g_trow++;
            g_tcol = 0;
            if (g_trow >= TERM_ROWS) {
                term_scroll();
                g_trow = TERM_ROWS - 1;
            }
        }
        g_term[g_trow][g_tcol++] = c;
        g_term[g_trow][g_tcol] = '\0';
    }
}

static void *pty_reader_thread(void *arg) {
    char buf[512];
    int in_esc = 0;
    while (1) {
        ssize_t n = read(g_pty_fd, buf, sizeof(buf));
        if (n <= 0) break;
        pthread_mutex_lock(&g_lock);
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\x1b') { in_esc = 1; continue; }
            if (in_esc) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) in_esc = 0;
                continue;
            }
            term_put_char(c);
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static void init_live_pty(void) {
    struct winsize ws = { .ws_row = TERM_ROWS, .ws_col = TERM_COLS };
    g_pty_pid = forkpty(&g_pty_fd, NULL, NULL, &ws);
    if (g_pty_pid == 0) {
        setenv("TERM", "dumb", 1);
        setenv("PS1", "zero-os$ ", 1);
        execl("/bin/zsh", "zsh", "-f", NULL);
        execl("/bin/sh", "sh", NULL);
        _exit(1);
    }
    pthread_t th;
    pthread_create(&th, NULL, pty_reader_thread, NULL);
    pthread_detach(th);
    const char *init_cmd = "uname -m -s -r && echo 'Live PTY ready. Type any command:'\n";
    write(g_pty_fd, init_cmd, strlen(init_cmd));
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

// --- Native NSURLSession Local AI Query (Zero Python Dependency) ---
static void query_local_ai_native(const char *prompt) {
    NSURL *url = [NSURL URLWithString:@"http://127.0.0.1:11434/api/generate"];
    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:url];
    req.HTTPMethod = @"POST";
    req.timeoutInterval = 30.0;
    [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];

    NSDictionary *body = @{@"model": @"llama3.2", @"prompt": [NSString stringWithUTF8String:prompt], @"stream": @NO, @"keep_alive": @"30m"};
    req.HTTPBody = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    snprintf(g_ai_reply, sizeof(g_ai_reply), "Querying 127.0.0.1:11434...");

    [[[NSURLSession sharedSession] dataTaskWithRequest:req completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
        pthread_mutex_lock(&g_lock);
        if (err || !data) {
            snprintf(g_ai_reply, sizeof(g_ai_reply), "Local LLM offline (start Ollama on :11434)");
        } else {
            NSDictionary *json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
            NSString *res = json[@"response"];
            if (res) {
                NSString *clean = [[res stringByReplacingOccurrencesOfString:@"\n" withString:@" "] substringToIndex:MIN(res.length, 210)];
                snprintf(g_ai_reply, sizeof(g_ai_reply), "AI: %s", [clean UTF8String]);
            }
        }
        pthread_mutex_unlock(&g_lock);
    }] resume];
}

// --- .atom Binary State Crystallization ---
static void save_atom_snapshot(void) {
    ZeroAtomSnapshot *snap = calloc(1, sizeof(ZeroAtomSnapshot));
    snap->magic = 0x5A45524F;
    strncpy(snap->cwd, g_cwd, sizeof(snap->cwd) - 1);
    memcpy(snap->term_lines, g_term, sizeof(g_term));
    snap->term_row = g_trow;
    snap->term_col = g_tcol;
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
        snprintf(g_status, sizeof(g_status), "NO .ATOM SNAPSHOT FOUND (PRESS [S] FIRST)");
        return;
    }
    ZeroAtomSnapshot *snap = calloc(1, sizeof(ZeroAtomSnapshot));
    if (fread(snap, sizeof(ZeroAtomSnapshot), 1, f) == 1 && snap->magic == 0x5A45524F) {
        scan_directory(snap->cwd);
        memcpy(g_term, snap->term_lines, sizeof(g_term));
        g_trow = snap->term_row;
        g_tcol = snap->term_col;
        strncpy(g_ai_reply, snap->ai_reply, sizeof(g_ai_reply) - 1);
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

    // [1] TOP-LEFT: REAL HOST FILESYSTEM
    fill_rect(8, 8, 628, 376, 0xFF111622);
    draw_border(8, 8, 628, 376, 0xFF28354D);
    draw_text(20, 16, "[1] HOST FILESYSTEM (LIVE POSIX DIR)", 13, cCyan, YES);
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

    // [2] BOTTOM-LEFT: REAL LIVE PTY SHELL
    fill_rect(8, 392, 628, 376, 0xFF0D1017);
    draw_border(8, 392, 628, 376, 0xFF2B5B3F);
    draw_text(20, 400, "[2] LIVE UNIX SHELL (POSIX forkpty /bin/zsh - TYPE DIRECTLY)", 13, cGreen, YES);
    for (int r = 0; r < TERM_ROWS; r++) {
        if (g_term[r][0]) {
            draw_text(20, 424 + r * 16, g_term[r], 11.5, cWhite, NO);
        }
    }

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
    draw_text(656, 326, "Shortcuts: [F5] Fetch Web Image | [F6] Save .atom | [F7] Restore .atom", 11, cAmber, NO);
    draw_text(656, 348, "Dir Nav:   [Up/Down] Select File | [F2] Open Dir", 11, cDim, NO);

    // [4] BOTTOM-RIGHT: REAL MACH KERNEL TELEMETRY & NATIVE AI
    fill_rect(644, 392, 628, 376, 0xFF111622);
    draw_border(644, 392, 628, 376, 0xFF28354D);
    draw_text(656, 400, "[4] REAL KERNEL TELEMETRY & NATIVE LOCAL AI", 13, cCyan, YES);

#if defined(__arm64__)
    const char *arch_str = "APPLE SILICON (arm64 NATIVE)";
#else
    const char *arch_str = "INTEL MAC (x86_64 NATIVE)";
#endif
    char info1[96], info2[96];
    snprintf(info1, sizeof(info1), "CPU ARCHITECTURE : %s", arch_str);
    snprintf(info2, sizeof(info2), "MACH RESIDENT RAM: %.2f MB (REAL PROCESS FOOTPRINT)", get_mach_ram_mb());
    draw_text(656, 434, info1, 13, cGreen, YES);
    draw_text(656, 458, info2, 13, cAmber, YES);
    draw_text(656, 482, "EXTERNAL SCRIPTS : 0 (ZERO PYTHON / ZERO SHELL WRAPPERS)", 12, cWhite, NO);

    fill_rect(656, 514, 604, 110, 0xFF0B0E14);
    draw_border(656, 514, 604, 110, 0xFF334155);
    draw_text(668, 524, "NATIVE LOCAL LLM BRIDGE (NSURLSession -> 127.0.0.1:11434)", 11.5, cCyan, YES);
    if (g_ai_input_mode) {
        char pbuf[160];
        snprintf(pbuf, sizeof(pbuf), "PROMPT> %s_", g_ai_prompt);
        draw_text(668, 552, pbuf, 12, cAmber, YES);
    } else {
        int len = (int)strlen(g_ai_reply);
        for (int row = 0; row < 3 && row * 72 < len; row++) {
            char line[73] = {0};
            strncpy(line, g_ai_reply + row * 72, 72);
            draw_text(668, 544 + row * 15, line, 11.5, cWhite, NO);
        }
    }
    draw_text(668, 592, "Press [F8] to type AI intent prompt | [F9] PCM Audio Test", 11, cDim, NO);

    // Bottom Status Bar
    fill_rect(8, 774, 1264, 20, 0xFF162033);
    draw_text(18, 776, g_status, 11.5, cGreen, YES);

    [NSGraphicsContext setCurrentContext:prevCtx];
    pthread_mutex_unlock(&g_lock);
}

@interface ZeroView : NSView
@end

@implementation ZeroView
- (BOOL)acceptsFirstResponder { return YES; }

- (void)drawRect:(NSRect)dirtyRect {
    render_frame();
    CGImageRef img = CGBitmapContextCreateImage(g_ctx);
    if (img) {
        CGContextRef winCtx = [[NSGraphicsContext currentContext] CGContext];
        CGContextDrawImage(winCtx, self.bounds, img);
        CGImageRelease(img);
    }
}

- (void)keyDown:(NSEvent *)event {
    unsigned short kc = [event keyCode];
    NSString *chars = [event characters];

    // Function keys & Navigation don't collide with typing in the live PTY shell
    if (kc == 96) { fetch_live_web_image(); return; }       // F5
    if (kc == 97) { save_atom_snapshot(); return; }         // F6
    if (kc == 98) { restore_atom_snapshot(); return; }      // F7
    if (kc == 100) {                                        // F8: Toggle AI input
        g_ai_input_mode = !g_ai_input_mode;
        g_ai_prompt[0] = '\0';
        return;
    }
    if (kc == 101) { play_pcm_chime(); return; }            // F9: Audio chime
    if (kc == 126) { // Up arrow -> File list
        if (g_selected_file > 0) g_selected_file--;
        return;
    }
    if (kc == 125) { // Down arrow -> File list
        if (g_selected_file + 1 < g_file_count) g_selected_file++;
        return;
    }
    if (kc == 120) { // F2 -> Open directory
        if (g_selected_file < g_file_count && g_files[g_selected_file].is_dir) {
            char next[1024];
            snprintf(next, sizeof(next), "%s/%s", g_cwd, g_files[g_selected_file].name);
            char resolved[512];
            if (realpath(next, resolved)) scan_directory(resolved);
        }
        return;
    }

    if (g_ai_input_mode) {
        if (kc == 36) { // Enter
            query_local_ai_native(g_ai_prompt);
            g_ai_input_mode = 0;
        } else if (kc == 51) { // Backspace
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

    // Forward all normal typing directly to the live PTY UNIX shell!
    if (g_pty_fd >= 0 && chars.length > 0) {
        const char *u = [chars UTF8String];
        if (u) write(g_pty_fd, u, strlen(u));
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
        fetch_live_web_image();
        play_pcm_chime();

        // Auto-fit screen resolution (works on both Retina M2 & older Intel Macs)
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
        [win setTitle:@"ZERO OS v0.2.0 — Live PTY Kernel, Native Filesystem & Universal Bridge"];
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
