#include <Cocoa/Cocoa.h>
#include <AudioToolbox/AudioToolbox.h>
#include <ImageIO/ImageIO.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

#define LOG_W 1120
#define LOG_H 720
#define WIDTH 2240
#define HEIGHT 1440
#define SR 44100
#define PANE_W 1080
#define PANE_H 620
#define IMG_W 840
#define IMG_H 520

static uint32_t* fb = NULL;
static uint32_t splash_tex[3][PANE_W * PANE_H];
static uint32_t desktop_tex[3][PANE_W * PANE_H];
static int has_custom_splash[3] = {0, 0, 0};
static int has_custom_desktop[3] = {0, 0, 0};

typedef struct {
    int os_state[3];
    int boot_timer[3];
    int file_owner;
    int file_x, file_y;
    int boot_all_queue;
    int zoom_mode;
    int news_idx;
    char img_title[64];
    char img_source[96];
    uint32_t img_buf[IMG_W * IMG_H];
} MultiOSState;

static struct {
    MultiOSState st;
    int dragging, drag_off_x, drag_off_y, hover_os, fetching;
    int chime_os, chime_frames, pmode, thinking, tick;
    char pbuf[128], status_msg[140];
    int plen;
    double p1, p2, p3, p4, ram_mb, frame_ms;
    int16_t scope[256];
} atom = {
    {{0, 0, 0}, {0, 0, 0}, 0, 420, 190, 0, 0, 0, "CONNECTING LIVE FEED...", "NASA / WORLD NEWS", {0}},
    0, 0, 0, -1, 0,
    -1, 0, 0, 0, 0, {0}, {"FETCHING LIVE WEB NEWS PHOTO DIRECTLY TO UNIFIED RAM..."}, 0,
    0, 0, 0, 0, 0.0, 0.0
};

void snap_file_to_os(int os_id) {
    atom.st.file_owner = os_id;
    if (os_id == 0) { atom.st.file_x = 28 + 410; atom.st.file_y = 28 + 185; }
    else if (os_id == 1) { atom.st.file_x = 1132 + 600; atom.st.file_y = 28 + 185; }
    else if (os_id == 2) { atom.st.file_x = 28 + 620; atom.st.file_y = 676 + 245; }
}

double now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

double get_real_ram_mb(void) {
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
        return (double)info.resident_size / (1024.0 * 1024.0);
    }
    return 0.0;
}

void fetch_live_news_image(int next_feed) {
    if (atom.fetching) return;
    atom.fetching = 1;
    if (next_feed) atom.st.news_idx = (atom.st.news_idx + 1) % 4;
    strcpy(atom.status_msg, "FETCHING LIVE WEB NEWS PHOTO TO UNIFIED RAM...");
    double t0 = now_ms();

    const char* urls[4] = {
        "https://images-assets.nasa.gov/image/PIA18033/PIA18033~medium.jpg",
        "https://images-assets.nasa.gov/image/iss065e000284/iss065e000284~medium.jpg",
        "https://upload.wikimedia.org/wikipedia/commons/thumb/9/97/The_Earth_seen_from_Apollo_17.jpg/800px-The_Earth_seen_from_Apollo_17.jpg",
        "https://picsum.photos/id/1015/840/520.jpg"
    };
    const char* titles[4] = {
        "NASA LIVE: DEEP SPACE OBSERVATORY",
        "ISS ORBITAL NEWS: EARTH HORIZON",
        "APOLLO ARCHIVE: BLUE MARBLE HD",
        "WORLD NEWS WIRE: LIVE PHOTO FEED"
    };
    const char* sources[4] = {
        "IMAGES-ASSETS.NASA.GOV",
        "IMAGES-ASSETS.NASA.GOV",
        "COMMONS.WIKIMEDIA.ORG",
        "LIVE-WIRE.NEWS"
    };

    int loaded = 0;
    long last_code = 0;
    size_t last_bytes = 0;

    for (int attempt = 0; attempt < 4; attempt++) {
        int idx = (atom.st.news_idx + attempt) % 4;
        @autoreleasepool {
            NSURL *url = [NSURL URLWithString:[NSString stringWithUTF8String:urls[idx]]];
            NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:url
                                                               cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                           timeoutInterval:8.0];
            [req setValue:@"ZeroOSKernel/1.0 (https://github.com/zero-os; dev@zero-os.org)" forHTTPHeaderField:@"User-Agent"];

            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            __block NSData *imgData = nil;
            __block NSInteger statusCode = 0;

            NSURLSessionDataTask *task = [[NSURLSession sharedSession] dataTaskWithRequest:req
                completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
                    if ([resp isKindOfClass:[NSHTTPURLResponse class]]) {
                        statusCode = [(NSHTTPURLResponse *)resp statusCode];
                    }
                    if (!err && statusCode == 200 && data.length > 1024) {
                        imgData = [data retain];
                    }
                    dispatch_semaphore_signal(sem);
                }];
            [task resume];
            dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 9 * NSEC_PER_SEC));

            last_code = (long)statusCode;
            last_bytes = imgData ? imgData.length : 0;

            if (imgData) {
                CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)imgData, NULL);
                CGImageRef cgImg = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
                if (cgImg) {
                    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
                    CGContextRef ctx = CGBitmapContextCreate(atom.st.img_buf, IMG_W, IMG_H, 8, IMG_W * 4, cs,
                                                             kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
                    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
                    CGContextDrawImage(ctx, CGRectMake(0, 0, IMG_W, IMG_H), cgImg);
                    CGContextRelease(ctx); CGColorSpaceRelease(cs); CGImageRelease(cgImg);
                    if (src) CFRelease(src);
                    [imgData release];

                    atom.st.news_idx = idx;
                    strcpy(atom.st.img_title, titles[idx]);
                    strcpy(atom.st.img_source, sources[idx]);
                    loaded = 1;
                    break;
                }
                if (src) CFRelease(src);
                [imgData release];
            }
        }
    }

    if (loaded) {
        time_t now = time(NULL);
        struct tm *lt = localtime(&now);
        snprintf(atom.status_msg, sizeof(atom.status_msg),
                 "LIVE PHOTO (%zu KB) LOADED AT %02d:%02d:%02d IN %.0f MS - DRAG TO ANY OS!",
                 last_bytes / 1024, lt->tm_hour, lt->tm_min, lt->tm_sec, now_ms() - t0);
        atom.chime_os = 10; atom.chime_frames = 7500;
    } else {
        snprintf(atom.status_msg, sizeof(atom.status_msg),
                 "HTTP %ld (%zu B) - CHECK INTERNET CONNECTION & PRESS [W] TO RETRY", last_code, last_bytes);
    }
    atom.fetching = 0;
}

void load_tex_file(const char* filename, uint32_t* dst, int* flag) {
    char path[512];
    snprintf(path, sizeof(path), "%s/zero_os/assets/%s", getenv("HOME"), filename);
    NSString *nsPath = [NSString stringWithUTF8String:path];
    if ([[NSFileManager defaultManager] fileExistsAtPath:nsPath]) {
        NSURL *url = [NSURL fileURLWithPath:nsPath];
        CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, NULL);
        CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
        if (img) {
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
            CGContextRef ctx = CGBitmapContextCreate(dst, PANE_W, PANE_H, 8, PANE_W * 4, cs,
                                                     kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
            CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
            CGContextDrawImage(ctx, CGRectMake(0, 0, PANE_W, PANE_H), img);
            CGContextRelease(ctx); CGColorSpaceRelease(cs); CGImageRelease(img);
            *flag = 1;
        }
        if (src) CFRelease(src);
    }
}

void load_all_assets(void) {
    const char* s_files[3] = {"mac9_splash.jpg", "xp_splash.jpg", "ubuntu_splash.jpg"};
    const char* d_files[3] = {"mac9_desktop.jpg", "xp_desktop.jpg", "ubuntu_desktop.jpg"};
    for (int k = 0; k < 3; k++) {
        load_tex_file(s_files[k], splash_tex[k], &has_custom_splash[k]);
        load_tex_file(d_files[k], desktop_tex[k], &has_custom_desktop[k]);
    }
}

void audio_cb(void *u, AudioQueueRef q, AudioQueueBufferRef b) {
    int16_t *pcm = (int16_t *)b->mAudioData;
    int n = b->mAudioDataBytesCapacity / 2;
    for (int i = 0; i < n; i++) {
        int16_t v = 0;
        if (atom.chime_frames > 0) {
            double prog = 1.0 - (double)atom.chime_frames / 36000.0;
            if (prog < 0.0) prog = 0.0;
            double env = (prog < 0.04) ? (prog / 0.04) : pow(1.0 - prog, 1.3);
            double f1 = 440, f2 = 550, f3 = 660, f4 = 880;
            if (atom.chime_os == 0) { f1 = 174.61; f2 = 261.63; f3 = 349.23; f4 = 440.00; }
            else if (atom.chime_os == 1) {
                if (prog < 0.22) { f1 = 622.25; f2 = 311.13; f3 = 466.16; f4 = 932.33; }
                else if (prog < 0.42) { f1 = 466.16; f2 = 233.08; f3 = 349.23; f4 = 698.46; }
                else if (prog < 0.65) { f1 = 415.30; f2 = 207.65; f3 = 311.13; f4 = 622.25; }
                else { f1 = 466.16; f2 = 311.13; f3 = 622.25; f4 = 932.33; }
            } else if (atom.chime_os == 2) {
                if (prog < 0.30) { f1 = 293.66; f2 = 440.00; f3 = 587.33; f4 = 880.00; }
                else if (prog < 0.60) { f1 = 329.63; f2 = 493.88; f3 = 659.25; f4 = 987.77; }
                else { f1 = 392.00; f2 = 587.33; f3 = 783.99; f4 = 1174.66; }
            } else if (atom.chime_os == 10) {
                f1 = 587.33; f2 = 880.00; f3 = 1174.66; f4 = 1760.00;
            }
            double sig = sin(atom.p1)*0.38 + sin(atom.p2)*0.30 + sin(atom.p3)*0.22 + sin(atom.p4)*0.12;
            v = (int16_t)(sig * 13500.0 * env);
            atom.p1 += 2.0 * M_PI * f1 / SR; atom.p2 += 2.0 * M_PI * f2 / SR;
            atom.p3 += 2.0 * M_PI * f3 / SR; atom.p4 += 2.0 * M_PI * f4 / SR;
            if (atom.p1 > 2.0*M_PI) atom.p1 -= 2.0*M_PI;
            if (atom.p2 > 2.0*M_PI) atom.p2 -= 2.0*M_PI;
            if (atom.p3 > 2.0*M_PI) atom.p3 -= 2.0*M_PI;
            if (atom.p4 > 2.0*M_PI) atom.p4 -= 2.0*M_PI;
            atom.chime_frames--;
        }
        pcm[i] = v;
        if (i < 256) atom.scope[i] = v;
    }
    b->mAudioDataByteSize = b->mAudioDataBytesCapacity;
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

void trigger_boot_os(int id) {
    if (id < 0 || id > 2) return;
    atom.st.os_state[id] = 1; atom.st.boot_timer[id] = 0;
    atom.chime_os = id; atom.chime_frames = 36000;
    const char* snds[3] = {"mac9_boot.mp3", "xp_boot.mp3", "ubuntu_boot.mp3"};
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "[ -f ~/zero_os/assets/%s ] && afplay ~/zero_os/assets/%s &", snds[id], snds[id]);
    system(cmd);
    const char* names[3] = {"MAC OS 9.2", "WINDOWS XP", "UBUNTU LINUX"};
    snprintf(atom.status_msg, sizeof(atom.status_msg), "BOOTING %s (2240x1440 RETINA HD DESKTOP)...", names[id]);
}

void save_multi_atom(void) {
    double t0 = now_ms();
    char path[256]; snprintf(path, sizeof(path), "%s/zero_os/studio_3os.atom", getenv("HOME"));
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fwrite(&atom.st, sizeof(MultiOSState), 1, fp);
        fclose(fp);
        snprintf(atom.status_msg, sizeof(atom.status_msg), "CRYSTALLIZED 3 OSES + LIVE NEWS PHOTO (%lu KB) IN %.2f MS",
                 sizeof(MultiOSState)/1024, now_ms() - t0);
        atom.chime_os = 10; atom.chime_frames = 7000;
    }
}

void load_multi_atom(void) {
    double t0 = now_ms();
    char path[256]; snprintf(path, sizeof(path), "%s/zero_os/studio_3os.atom", getenv("HOME"));
    FILE *fp = fopen(path, "rb");
    if (fp) {
        fread(&atom.st, sizeof(MultiOSState), 1, fp);
        fclose(fp);
        snprintf(atom.status_msg, sizeof(atom.status_msg), "RESTORED ALL 3 OSES + LIVE NEWS PHOTO IN %.2f MS", now_ms() - t0);
        atom.chime_os = 10; atom.chime_frames = 7000;
    }
}

void run_local_ai(const char* intent) {
    atom.thinking = 1;
    char cmd[2400];
    snprintf(cmd, sizeof(cmd),
        "python3 -c 'import urllib.request, json; "
        "req = urllib.request.Request(\"http://127.0.0.1:8089/completion\", "
        "data=json.dumps({\"prompt\":\"Map command to int: 1=BOOT_MAC 2=BOOT_WIN 3=BOOT_LINUX 4=BOOT_ALL 5=NEXT_NEWS 6=EXPAND 7=SAVE 8=LOAD\\n"
        "Cmd: BOOT ALL\\nRes: 4\\nCmd: NEXT NEWS\\nRes: 5\\nCmd: EXPAND IMAGE\\nRes: 6\\n"
        "Cmd: SAVE\\nRes: 7\\nCmd: LOAD\\nRes: 8\\n"
        "Cmd: %s\\nRes:\",\"n_predict\":8,\"temperature\":0.1,\"stop\":[\"\\n\"]}).encode(), "
        "headers={\"Content-Type\":\"application/json\"}); "
        "res = json.loads(urllib.request.urlopen(req, timeout=5).read()); "
        "print(res.get(\"content\",\"\").strip())'", intent);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char out[64] = {0};
        if (fgets(out, sizeof(out), fp)) {
            int c = atoi(out);
            if (c == 1) trigger_boot_os(0);
            else if (c == 2) trigger_boot_os(1);
            else if (c == 3) trigger_boot_os(2);
            else if (c == 4) { atom.st.boot_all_queue = 1; trigger_boot_os(0); }
            else if (c == 5) dispatch_async(dispatch_get_global_queue(0, 0), ^{ fetch_live_news_image(1); });
            else if (c == 6) atom.st.zoom_mode = !atom.st.zoom_mode;
            else if (c == 7) save_multi_atom();
            else if (c == 8) load_multi_atom();
        }
        pclose(fp);
    }
    atom.thinking = 0;
}

void rect(int x0, int y0, int w, int h, uint32_t c) {
    for (int y = y0; y < y0 + h; y++) {
        if ((unsigned)y >= HEIGHT) continue;
        for (int x = x0; x < x0 + w; x++) if ((unsigned)x < WIDTH) fb[y * WIDTH + x] = c;
    }
}

void stroke_rect(int x0, int y0, int w, int h, int t, uint32_t c) {
    rect(x0, y0, w, t, c); rect(x0, y0 + h - t, w, t, c);
    rect(x0, y0, t, h, c); rect(x0 + w - t, y0, t, h, c);
}

void bevel_box(int x, int y, int w, int h, uint32_t base, uint32_t hi, uint32_t lo) {
    rect(x, y, w, h, base);
    rect(x, y, w, 2, hi); rect(x, y, 2, h, hi);
    rect(x, y + h - 2, w, 2, lo); rect(x + w - 2, y, 2, h, lo);
}

void fill_circle(int cx, int cy, int r, uint32_t c) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x*x + y*y <= r*r) rect(cx + x, cy + y, 1, 1, c);
}

void blit_tex(const uint32_t* tex, int dx, int dy) {
    for (int y = 0; y < PANE_H; y++)
        for (int x = 0; x < PANE_W; x++)
            fb[(dy + y) * WIDTH + (dx + x)] = tex[y * PANE_W + x];
}

void draw_hd_text(int x, int y, const char* str, CGFloat ptSize, int bold, uint32_t argb) {
    NSString *s = [NSString stringWithUTF8String:str];
    if (!s) return;
    CGFloat r = ((argb >> 16) & 0xFF) / 255.0;
    CGFloat g = ((argb >> 8) & 0xFF) / 255.0;
    CGFloat b = (argb & 0xFF) / 255.0;
    NSColor *col = [NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0];
    NSFont *font = bold ? [NSFont boldSystemFontOfSize:ptSize] : [NSFont systemFontOfSize:ptSize];
    NSDictionary *attrs = @{NSFontAttributeName: font, NSForegroundColorAttributeName: col};
    [s drawAtPoint:NSMakePoint(x, y) withAttributes:attrs];
}

void blit_news_scaled(int dx, int dy, int dw, int dh) {
    for (int py = 0; py < dh; py++) {
        int out_y = dy + py;
        if ((unsigned)out_y >= HEIGHT) continue;
        int sy = (py * IMG_H) / dh;
        if (sy >= IMG_H) sy = IMG_H - 1;
        for (int px = 0; px < dw; px++) {
            int out_x = dx + px;
            if ((unsigned)out_x >= WIDTH) continue;
            int sx = (px * IMG_W) / dw;
            if (sx >= IMG_W) sx = IMG_W - 1;
            fb[out_y * WIDTH + out_x] = atom.st.img_buf[sy * IMG_W + sx];
        }
    }
}

void get_os_window_inner_rect(int os_id, int *ox, int *oy, int *ow, int *oh) {
    if (os_id == 0) {
        *ox = 28 + 64; *oy = 28 + 122; *ow = 692; *oh = 400;
    } else if (os_id == 1) {
        *ox = 1132 + 398; *oy = 28 + 112; *ow = 574; *oh = 402;
    } else {
        *ox = 28 + 136; *oy = 676 + 122; *ow = 852; *oh = 436;
    }
}

void draw_live_news_viewer(void) {
    int owner = atom.st.file_owner;
    if (atom.st.os_state[owner] != 2 && !atom.dragging) return;

    if (atom.st.zoom_mode && !atom.dragging) {
        int wx, wy, ww, wh;
        get_os_window_inner_rect(owner, &wx, &wy, &ww, &wh);
        rect(wx, wy, ww, wh, 0xFF090D16);
        rect(wx, wy, ww, 44, 0xFF0284C7);
        blit_news_scaled(wx + 6, wy + 48, ww - 12, wh - 54);
        stroke_rect(wx, wy, ww, wh, 3, 0xFF00E5FF);
    } else {
        int x = atom.st.file_x, y = atom.st.file_y;
        int cw = 320, ch = 236;
        int lift = atom.dragging ? 18 : 8;
        rect(x + lift, y + lift, cw, ch, atom.dragging ? 0x99000000 : 0x55000000);
        rect(x, y, cw, ch, 0xFF0F172A);
        stroke_rect(x, y, cw, ch, atom.dragging ? 4 : 2, atom.dragging ? 0xFF00E5FF : 0xFF38BDF8);
        rect(x, y, cw, 34, atom.dragging ? 0xFF0284C7 : 0xFF1E293B);
        blit_news_scaled(x + 8, y + 38, cw - 16, ch - 46);
    }
}

void render_macos9_bg(int x, int y, int w, int h) {
    int st = atom.st.os_state[0];
    if (st == 0) { rect(x, y, w, h, 0xFF0F131A); return; }
    if (st == 1) {
        if (has_custom_splash[0]) { blit_tex(splash_tex[0], x, y); return; }
        rect(x, y, w, h, 0xFF686A9C);
        int wx = x + 170, wy = y + 60, ww = 740, wh = 490;
        rect(wx + 8, wy + 8, ww, wh, 0xFF333555);
        bevel_box(wx, wy, ww, wh, 0xFFDDDDDD, 0xFFFFFFFF, 0xFF888888);
        stroke_rect(wx, wy, ww, wh, 2, 0xFF111111);
        rect(wx + 48, wy + 40, ww - 96, 300, 0xFFFFFFFF);
        stroke_rect(wx + 48, wy + 40, ww - 96, 300, 2, 0xFF333333);
        int fx = wx + 306, fy = wy + 70;
        rect(fx, fy, 64, 108, 0xFF3B66B8); rect(fx + 64, fy, 64, 108, 0xFF7CA5E8);
        rect(fx + 28, fy + 28, 8, 14, 0xFF111111); rect(fx + 92, fy + 28, 8, 14, 0xFF111111);
        rect(fx + 60, fy - 8, 6, 68, 0xFF111111); rect(fx + 44, fy + 56, 20, 6, 0xFF111111);
        rect(fx + 60, fy + 56, 6, 36, 0xFF111111); rect(fx + 28, fy + 84, 72, 6, 0xFF111111);
        int pbx = wx + 180, pby = wy + 410, pbw = 380;
        rect(pbx, pby, pbw, 24, 0xFFBBBBBB); stroke_rect(pbx, pby, pbw, 24, 2, 0xFF222222);
        int fill = (atom.st.boot_timer[0] * (pbw - 8)) / 55; if (fill > pbw - 8) fill = pbw - 8;
        rect(pbx + 4, pby + 4, fill, 16, 0xFF4F6BED);
        return;
    }
    if (has_custom_desktop[0]) { blit_tex(desktop_tex[0], x, y); return; }

    for (int py = 0; py < h; py++)
        for (int px = 0; px < w; px++)
            fb[(y + py) * WIDTH + (x + px)] = ((px + py) & 1) ? 0xFF686A9C : 0xFF626494;

    bevel_box(x, y, w, 44, 0xFFEEEEEE, 0xFFFFFFFF, 0xFFAAAAAA);
    rect(x, y + 44, w, 2, 0xFF111111);
    uint32_t apple_cols[6] = {0xFF61BB46, 0xFFFDB827, 0xFFF5821F, 0xFFE03A3E, 0xFF963D97, 0xFF009DDC};
    for (int c = 0; c < 6; c++) rect(x + 22, y + 10 + c * 4, 22, 4, apple_cols[c]);

    int wx = x + 46, wy = y + 74, ww = 750, wh = 490;
    rect(wx + 8, wy + 8, ww, wh, 0x55000000);
    bevel_box(wx, wy, ww, wh, 0xFFDDDDDD, 0xFFFFFFFF, 0xFF888888);
    stroke_rect(wx, wy, ww, wh, 2, 0xFF111111);
    for (int ly = wy + 10; ly < wy + 36; ly += 4) rect(wx + 46, ly, ww - 110, 2, 0xFF777777);
    bevel_box(wx + 14, wy + 12, 22, 22, 0xFFDDDDDD, 0xFFFFFFFF, 0xFF666666);
    bevel_box(wx + ww - 60, wy + 12, 22, 22, 0xFFDDDDDD, 0xFFFFFFFF, 0xFF666666);
    bevel_box(wx + ww - 32, wy + 12, 22, 22, 0xFFDDDDDD, 0xFFFFFFFF, 0xFF666666);
    rect(wx + 250, wy + 6, 250, 32, 0xFFDDDDDD);

    rect(wx + 14, wy + 46, ww - 54, wh - 84, 0xFFFFFFFF);
    stroke_rect(wx + 14, wy + 46, ww - 54, wh - 84, 2, 0xFF555555);
    bevel_box(wx + 16, wy + 48, ww - 58, 36, 0xFFE5E7EB, 0xFFFFFFFF, 0xFF9CA3AF);
    rect(wx + ww - 40, wy + 46, 26, wh - 84, 0xFFCCCCCC);
    stroke_rect(wx + ww - 40, wy + 46, 26, wh - 84, 1, 0xFF555555);
    bevel_box(wx + ww - 38, wy + 74, 22, 160, 0xFF9999CC, 0xFFCCCCFF, 0xFF555588);
    rect(wx + 14, wy + wh - 38, ww - 54, 24, 0xFFCCCCCC);
    stroke_rect(wx + 14, wy + wh - 38, ww - 54, 24, 1, 0xFF555555);

    int fx_pos[3] = {wx + 48, wx + 200, wx + 352};
    uint32_t f_cols[3] = {0xFF93C5FD, 0xFF86EFAC, 0xFFFDE047};
    for (int f = 0; f < 3; f++) {
        rect(fx_pos[f], wy + 112, 34, 10, f_cols[f]);
        bevel_box(fx_pos[f], wy + 120, 84, 58, f_cols[f], 0xFFFFFFFF, 0xFF1E3A8A);
        stroke_rect(fx_pos[f], wy + 120, 84, 58, 2, 0xFF1E293B);
    }

    bevel_box(x + w - 144, y + 76, 84, 64, 0xFFD1D5DB, 0xFFFFFFFF, 0xFF4B5563);
    stroke_rect(x + w - 144, y + 76, 84, 64, 2, 0xFF111111);
    rect(x + w - 130, y + 124, 18, 6, 0xFF22C55E);
    bevel_box(x + w - 132, y + h - 136, 62, 74, 0xFF9CA3AF, 0xFFE5E7EB, 0xFF374151);
    stroke_rect(x + w - 132, y + h - 136, 62, 74, 2, 0xFF111111);
    bevel_box(x, y + h - 42, 260, 42, 0xFFDDDDDD, 0xFFFFFFFF, 0xFF666666);
    stroke_rect(x, y + h - 42, 260, 42, 2, 0xFF111111);
}

void render_winxp_bg(int x, int y, int w, int h) {
    int st = atom.st.os_state[1];
    if (st == 0) { rect(x, y, w, h, 0xFF0F131A); return; }
    if (st == 1) {
        if (has_custom_splash[1]) { blit_tex(splash_tex[1], x, y); return; }
        rect(x, y, w, h, 0xFF000000);
        int lx = x + 500, ly = y + 96;
        rect(lx, ly, 68, 60, 0xFFE84C22); rect(lx + 76, ly + 8, 68, 60, 0xFF7FBA00);
        rect(lx - 8, ly + 68, 68, 60, 0xFF00A4EF); rect(lx + 68, ly + 76, 68, 60, 0xFFFFB900);
        int bx = x + 360, by = y + 440, bw = 360;
        stroke_rect(bx, by, bw, 30, 2, 0xFF777777);
        int pos = (atom.st.boot_timer[1] * 8) % (bw - 80);
        for (int b = 0; b < 3; b++) rect(bx + 6 + pos + b * 24, by + 6, 18, 18, 0xFF3B82F6);
        return;
    }
    if (has_custom_desktop[1]) { blit_tex(desktop_tex[1], x, y); return; }

    for (int py = 0; py < h - 52; py++) {
        uint8_t r = 32 + (py * 95) / (h - 52);
        uint8_t g = 102 + (py * 105) / (h - 52);
        uint8_t b = 215 + (py * 38) / (h - 52);
        rect(x, y + py, w, 1, 0xFF000000 | (r << 16) | (g << 8) | b);
    }
    fill_circle(x + 260, y + 120, 42, 0xCCEAF4FF); fill_circle(x + 305, y + 115, 52, 0xDDFFFFFF); fill_circle(x + 355, y + 125, 38, 0xCCEAF4FF);
    fill_circle(x + 760, y + 95, 36, 0xCCEAF4FF); fill_circle(x + 800, y + 90, 44, 0xDDFFFFFF);

    for (int px = 0; px < w; px++) {
        int hill_y = (int)(h - 245 + sin(px * 0.0042 + 0.5) * 48.0 + cos(px * 0.009) * 16.0);
        for (int py = hill_y; py < h - 52; py++) {
            int depth = py - hill_y;
            uint8_t r = (depth < 40) ? (95 - depth / 2) : (56 - (depth - 40) / 12);
            uint8_t g = (depth < 40) ? (185 - depth / 2) : (148 - (depth - 40) / 5);
            uint8_t b = (depth < 40) ? (45 - depth / 4) : (32 - (depth - 40) / 14);
            fb[(y + py) * WIDTH + (x + px)] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }

    for (int ty = 0; ty < 52; ty++) {
        uint8_t b = 235 - abs(ty - 20) * 2;
        rect(x, y + h - 52 + ty, w, 1, 0xFF1F54C8 | b);
    }
    rect(x, y + h - 52, w, 3, 0xFF7DD3FC);
    rect(x, y + h - 52, 176, 52, 0xFF389E34);
    rect(x, y + h - 52, 176, 3, 0xFF86EFAC);
    rect(x + 18, y + h - 38, 11, 11, 0xFFE84C22); rect(x + 31, y + h - 36, 11, 11, 0xFF7FBA00);
    rect(x + 16, y + h - 25, 11, 11, 0xFF00A4EF); rect(x + 29, y + h - 23, 11, 11, 0xFFFFB900);
    rect(x + w - 170, y + h - 52, 170, 52, 0xFF1290E8);
    rect(x + w - 170, y + h - 52, 2, 52, 0xFF0C4A6E);

    bevel_box(x + 36, y + 40, 68, 52, 0xFF93C5FD, 0xFFFFFFFF, 0xFF1E3A8A);
    stroke_rect(x + 36, y + 40, 68, 52, 2, 0xFF1E3A8A);
    bevel_box(x + 40, y + 150, 60, 66, 0xFFE2E8F0, 0xFFFFFFFF, 0xFF475569);

    int wx = x + 160, wy = y + 54, ww = 820, wh = 465;
    rect(wx + 8, wy + 8, ww, wh, 0x55000000);
    rect(wx, wy, ww, wh, 0xFFECE9D8);
    stroke_rect(wx, wy, ww, wh, 4, 0xFF0054E3);
    for (int ty = 0; ty < 52; ty++) {
        uint8_t g = 95 + (26 - abs(ty - 26));
        rect(wx, wy + ty, ww, 1, 0xFF0000E3 | (g << 8));
    }
    bevel_box(wx + ww - 122, wy + 10, 32, 32, 0xFF2563EB, 0xFF93C5FD, 0xFF1E3A8A);
    bevel_box(wx + ww - 84, wy + 10, 32, 32, 0xFF2563EB, 0xFF93C5FD, 0xFF1E3A8A);
    bevel_box(wx + ww - 46, wy + 10, 32, 32, 0xFFDC2626, 0xFFFCA5A5, 0xFF7F1D1D);

    rect(wx + 6, wy + 52, 230, wh - 58, 0xFF6B8EE6);
    bevel_box(wx + 20, wy + 72, 202, 130, 0xFFD6DFF7, 0xFFFFFFFF, 0xFF3B5998);
    rect(wx + 20, wy + 72, 202, 32, 0xFF2157D5);
    rect(wx + 236, wy + 52, ww - 242, wh - 58, 0xFFFFFFFF);
    bevel_box(wx + 266, wy + 125, 76, 52, 0xFFCBD5E1, 0xFFFFFFFF, 0xFF475569);
    rect(wx + 276, wy + 162, 24, 6, 0xFF22C55E);
}

void render_ubuntu_bg(int x, int y, int w, int h) {
    int st = atom.st.os_state[2];
    if (st == 0) { rect(x, y, w, h, 0xFF0F131A); return; }
    if (st == 1) {
        if (has_custom_splash[2]) { blit_tex(splash_tex[2], x, y); return; }
        rect(x, y, w, h, 0xFF2C001E);
        fill_circle(x + 655, y + 266, 22, 0xFFE95420);
        fill_circle(x + 655, y + 266, 10, 0xFFFFFFFF);
        int active_dots = ((atom.st.boot_timer[2] / 10) % 5) + 1;
        for (int d = 0; d < 5; d++) {
            uint32_t dc = (d < active_dots) ? 0xFFE95420 : 0xFFFFFFFF;
            fill_circle(x + 450 + d * 36, y + 345, 6, dc);
        }
        return;
    }
    if (has_custom_desktop[2]) { blit_tex(desktop_tex[2], x, y); return; }

    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            double dist = sqrt((px - 760)*(px - 760) + (py - 320)*(py - 320));
            uint8_t r = 46 + (py * 68) / h + (dist < 280 ? (int)((280 - dist) * 0.25) : 0);
            uint8_t g = 4 + (py * 24) / h + (dist < 280 ? (int)((280 - dist) * 0.08) : 0);
            uint8_t b = 34 + (py * 52) / h;
            fb[(y + py) * WIDTH + (x + px)] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }
    rect(x, y, w, 44, 0xFF141417);
    rect(x, y + 44, w, 1, 0xFF27272A);

    rect(x, y + 44, 88, h - 44, 0xCC180412);
    rect(x + 88, y + 44, 1, h - 44, 0x44FFFFFF);
    uint32_t dock_cols[4] = {0xFFE95420, 0xFF0EA5E9, 0xFF10B981, 0xFF6366F1};
    for (int d = 0; d < 4; d++) {
        int dy = y + 68 + d * 78;
        rect(x + 16, dy, 56, 56, dock_cols[d]);
        stroke_rect(x + 16, dy, 56, 56, 2, 0x55FFFFFF);
        if (d < 3) fill_circle(x + 7, dy + 28, 4, 0xFFE95420);
    }

    int wx = x + 132, wy = y + 72, ww = 860, wh = 490;
    rect(wx + 10, wy + 10, ww, wh, 0x77000000);
    rect(wx, wy, ww, wh, 0xFF2B0920);
    stroke_rect(wx, wy, ww, wh, 2, 0xFF52525B);
    rect(wx, wy, ww, 48, 0xFF27272A);
    fill_circle(wx + ww - 28, wy + 24, 10, 0xFFE95420);
    fill_circle(wx + ww - 60, wy + 24, 10, 0xFF52525B);
    fill_circle(wx + ww - 92, wy + 24, 10, 0xFF52525B);
}

void render_control_bg(int x, int y, int w, int h) {
    rect(x, y, w, h, 0xFF0B0F17);
    stroke_rect(x, y, w, h, 4, 0xFF00E5FF);
    rect(x, y, w, 48, 0xFF0F172A);
    rect(x + 36, y + 210, w - 72, 140, 0xFF131924);
    stroke_rect(x + 36, y + 210, w - 72, 140, 2, 0xFF334155);
    for (int i = 0; i < 245; i++) rect(x + 48 + i * 4, y + 286 - (atom.scope[i] / 210), 4, 5, 0xFF00FF88);
}

void render_all_hd_labels(void) {
    int x1 = 28, y1 = 28;
    if (atom.st.os_state[0] == 0) {
        draw_hd_text(x1 + 310, y1 + 260, "[1] MAC OS 9.2 (STANDBY)", 32, 1, 0xFF64748B);
        draw_hd_text(x1 + 360, y1 + 315, "CLICK OR PRESS [1] TO BOOT", 22, 0, 0xFF475569);
    } else if (atom.st.os_state[0] == 1 && !has_custom_splash[0]) {
        draw_hd_text(x1 + 390, y1 + 265, "Mac OS 9.2", 52, 1, 0xFF111111);
        draw_hd_text(x1 + 465, y1 + 425, "Starting Up...", 22, 1, 0xFF111111);
    } else if (atom.st.os_state[0] == 2 && !has_custom_desktop[0]) {
        draw_hd_text(x1 + 64, y1 + 10, "File    Edit    View    Special    Help", 20, 1, 0xFF111111);
        draw_hd_text(x1 + PANE_W - 155, y1 + 10, "Mac OS 9.2", 20, 1, 0xFF111111);
        draw_hd_text(x1 + 312, y1 + 85, "Macintosh HD", 20, 1, 0xFF111111);
        if (!(atom.st.zoom_mode && atom.st.file_owner == 0)) {
            draw_hd_text(x1 + 78, y1 + 128, "3 items    18.4 GB available", 18, 0, 0xFF333333);
            draw_hd_text(x1 + 98, y1 + 262, "System", 18, 0, 0xFF111111);
            draw_hd_text(x1 + 240, y1 + 262, "Apps OS9", 18, 0, 0xFF111111);
            draw_hd_text(x1 + 392, y1 + 262, "Documents", 18, 0, 0xFF111111);
        }
        draw_hd_text(x1 + PANE_W - 152, y1 + 148, "Macintosh HD", 17, 1, 0xFFFFFFFF);
        draw_hd_text(x1 + PANE_W - 125, y1 + PANE_H - 54, "Trash", 17, 1, 0xFFFFFFFF);
        draw_hd_text(x1 + 20, y1 + PANE_H - 32, "Mac OS 9.2 Control Strip", 16, 1, 0xFF333333);
    }

    int x2 = 1132, y2 = 28;
    if (atom.st.os_state[1] == 0) {
        draw_hd_text(x2 + 310, y2 + 260, "[2] WINDOWS XP (STANDBY)", 32, 1, 0xFF64748B);
        draw_hd_text(x2 + 360, y2 + 315, "CLICK OR PRESS [2] TO BOOT", 22, 0, 0xFF475569);
    } else if (atom.st.os_state[1] == 1 && !has_custom_splash[1]) {
        draw_hd_text(x2 + 335, y2 + 235, "Microsoft", 26, 0, 0xFFCCCCCC);
        draw_hd_text(x2 + 335, y2 + 265, "Windows", 76, 1, 0xFFFFFFFF);
        draw_hd_text(x2 + 690, y2 + 262, "xp", 44, 1, 0xFFFF6600);
    } else if (atom.st.os_state[1] == 2 && !has_custom_desktop[1]) {
        draw_hd_text(x2 + 54, y2 + PANE_H - 38, "start", 25, 1, 0xFFFFFFFF);
        draw_hd_text(x2 + PANE_W - 148, y2 + PANE_H - 36, "19:30  JP", 19, 1, 0xFFFFFFFF);
        draw_hd_text(x2 + 26, y2 + 98, "My Computer", 16, 1, 0xFFFFFFFF);
        draw_hd_text(x2 + 28, y2 + 222, "Recycle Bin", 16, 1, 0xFFFFFFFF);
        draw_hd_text(x2 + 182, y2 + 68, "My Computer - ZeroOS Unified Drive", 20, 1, 0xFFFFFFFF);
        draw_hd_text(x2 + 194, y2 + 132, "System Tasks", 18, 1, 0xFFFFFFFF);
        draw_hd_text(x2 + 194, y2 + 174, "View system info", 16, 0, 0xFF1E3A8A);
        draw_hd_text(x2 + 194, y2 + 202, "ZeroOS Bridge Link", 16, 1, 0xFF1E3A8A);
        if (!(atom.st.zoom_mode && atom.st.file_owner == 1)) {
            draw_hd_text(x2 + 416, y2 + 124, "Hard Disk Drives", 18, 1, 0xFF1E293B);
            draw_hd_text(x2 + 422, y2 + 240, "Local Disk (C:)", 17, 1, 0xFF111111);
        }
    }

    int x3 = 28, y3 = 676;
    if (atom.st.os_state[2] == 0) {
        draw_hd_text(x3 + 300, y3 + 260, "[3] UBUNTU LINUX (STANDBY)", 32, 1, 0xFF64748B);
        draw_hd_text(x3 + 360, y3 + 315, "CLICK OR PRESS [3] TO BOOT", 22, 0, 0xFF475569);
    } else if (atom.st.os_state[2] == 1 && !has_custom_splash[2]) {
        draw_hd_text(x3 + 410, y3 + 235, "ubuntu", 64, 1, 0xFFFFFFFF);
    } else if (atom.st.os_state[2] == 2 && !has_custom_desktop[2]) {
        draw_hd_text(x3 + 24, y3 + 10, "Activities      Terminal", 20, 1, 0xFFFFFFFF);
        draw_hd_text(x3 + 470, y3 + 10, "Nov 19  19:30", 20, 1, 0xFFFFFFFF);
        draw_hd_text(x3 + PANE_W - 190, y3 + 10, "Ubuntu 24.04 LTS", 20, 1, 0xFFE95420);
        draw_hd_text(x3 + 156, y3 + 84, "zero@m2-ubuntu: ~/zero_bridge", 20, 1, 0xFFFFFFFF);
        if (!(atom.st.zoom_mode && atom.st.file_owner == 2)) {
            draw_hd_text(x3 + 162, y3 + 146, "$ uname -srm", 22, 1, 0xFF4ADE80);
            draw_hd_text(x3 + 162, y3 + 180, "Linux 6.8.0-zero-native aarch64", 22, 0, 0xFFE2E8F0);
            draw_hd_text(x3 + 162, y3 + 228, "$ mount | grep zero_atom", 22, 1, 0xFF4ADE80);
            draw_hd_text(x3 + 162, y3 + 262, "zero_unified_mem on /mnt/bridge (rw)", 22, 0, 0xFF38BDF8);
            draw_hd_text(x3 + 162, y3 + 310, "$ free -h", 22, 1, 0xFF4ADE80);
            draw_hd_text(x3 + 162, y3 + 344, "Mem: Zero-Copy Shared Framebuffer Active", 22, 0, 0xFFFBBF24);
        }
    }

    int x4 = 1132, y4 = 676;
    draw_hd_text(x4 + 24, y4 + 12, "[4] ZERO OS KERNEL TELEMETRY (2240x1440 RETINA)", 20, 1, 0xFF00E5FF);
    char b1[80], b2[80];
    int active_cnt = (atom.st.os_state[0]==2) + (atom.st.os_state[1]==2) + (atom.st.os_state[2]==2);
    snprintf(b1, sizeof(b1), "ACTIVE OS ENVIRONMENTS : %d / 3 ONLINE", active_cnt);
    snprintf(b2, sizeof(b2), "REAL MACH KERNEL RAM   : %.1f MB (%.2f MS)", atom.ram_mb, atom.frame_ms);
    draw_hd_text(x4 + 36, y4 + 78, b1, 32, 1, 0xFF00FF88);
    draw_hd_text(x4 + 36, y4 + 130, b2, 32, 1, 0xFFFFCC00);
    draw_hd_text(x4 + 52, y4 + 222, "DIRECT PCM BOOT CHIME SYNTHESIZER (44.1 KHZ)", 18, 1, 0xFF94A3B8);

    draw_hd_text(x4 + 36, y4 + 380, "[B] BOOT ALL 3 OSES    [1/2/3] INDIVIDUAL BOOT", 22, 1, 0xFF38BDF8);
    draw_hd_text(x4 + 36, y4 + 422, "[W] FETCH NEXT LIVE WEB NEWS PHOTO", 22, 1, 0xFFFBBF24);
    draw_hd_text(x4 + 36, y4 + 464, "[DRAG] MOVE TO ANY OS  ->  [Z / DBL-CLICK] EXPAND", 22, 1, 0xFF00E5FF);
    draw_hd_text(x4 + 36, y4 + 506, "[S] SAVE .ATOM SNAPSHOT    [L] INSTANT RESTORE", 22, 1, 0xFF4ADE80);
    draw_hd_text(x4 + 36, y4 + 548, "[C] RESET ALL    [/] LOCAL AI VOICE/TEXT INTENT", 22, 0, 0xFF94A3B8);

    int owner = atom.st.file_owner;
    if (atom.st.os_state[owner] == 2 || atom.dragging) {
        const char* os_short[3] = {"MAC OS 9.2", "WINDOWS XP", "UBUNTU"};
        if (atom.st.zoom_mode && !atom.dragging) {
            int wx, wy, ww, wh;
            get_os_window_inner_rect(owner, &wx, &wy, &ww, &wh);
            char hdr[120];
            snprintf(hdr, sizeof(hdr), "[%s] %s", os_short[owner], atom.st.img_title);
            draw_hd_text(wx + 14, wy + 11, hdr, 16, 1, 0xFFFFFFFF);
            draw_hd_text(wx + ww - 115, wy + 12, "[Z] SHRINK", 15, 1, 0xFFFFCC00);
        } else {
            char card_hdr[96];
            snprintf(card_hdr, sizeof(card_hdr), "LIVE: %s", atom.st.img_title);
            draw_hd_text(atom.st.file_x + 12, atom.st.file_y + 7, card_hdr, 14, 1, 0xFFFFFFFF);
        }
    }

    draw_hd_text(32, HEIGHT - 105, atom.status_msg, 28, 1, 0xFFFFCC00);
    if (atom.pmode || atom.thinking) {
        char prompt_line[160];
        snprintf(prompt_line, sizeof(prompt_line), "> %s", atom.pbuf);
        draw_hd_text(44, HEIGHT - 54, prompt_line, 28, 1, 0xFFFFFFFF);
    }
}

int get_os_pane_at(int x, int y) {
    if (x >= 28 && x <= 28 + PANE_W && y >= 28 && y <= 28 + PANE_H) return 0;
    if (x >= 1132 && x <= 1132 + PANE_W && y >= 28 && y <= 28 + PANE_H) return 1;
    if (x >= 28 && x <= 28 + PANE_W && y >= 676 && y <= 676 + PANE_H) return 2;
    return -1;
}

@interface ZView : NSView
@end
@implementation ZView
- (BOOL)acceptsFirstResponder { return YES; }

- (void)mouseDown:(NSEvent *)e {
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    int x = (int)(p.x * 2.0), y = HEIGHT - (int)(p.y * 2.0);
    int owner = atom.st.file_owner;

    if (atom.st.os_state[owner] == 2) {
        int hit = 0;
        if (atom.st.zoom_mode) {
            int wx, wy, ww, wh;
            get_os_window_inner_rect(owner, &wx, &wy, &ww, &wh);
            if (x >= wx && x <= wx + ww && y >= wy && y <= wy + wh) hit = 1;
        } else {
            if (x >= atom.st.file_x && x <= atom.st.file_x + 320 &&
                y >= atom.st.file_y && y <= atom.st.file_y + 236) hit = 1;
        }

        if (hit) {
            if ([e clickCount] == 2) {
                atom.st.zoom_mode = !atom.st.zoom_mode;
                const char* names[3] = {"MAC OS 9.2", "WINDOWS XP", "UBUNTU LINUX"};
                snprintf(atom.status_msg, sizeof(atom.status_msg),
                         atom.st.zoom_mode ? "EXPANDED LIVE NEWS PHOTO INSIDE %s WINDOW!" : "COLLAPSED TO FLOATING CARD",
                         names[owner]);
                atom.chime_os = 10; atom.chime_frames = 5500;
                return;
            }
            atom.dragging = 1;
            if (atom.st.zoom_mode) {
                atom.st.file_x = x - 160;
                atom.st.file_y = y - 118;
                atom.drag_off_x = 160;
                atom.drag_off_y = 118;
            } else {
                atom.drag_off_x = x - atom.st.file_x;
                atom.drag_off_y = y - atom.st.file_y;
            }
            atom.hover_os = owner;
            snprintf(atom.status_msg, sizeof(atom.status_msg), "DRAGGING LIVE NEWS PHOTO ACROSS OS BOUNDARIES...");
            return;
        }
    }

    int target = get_os_pane_at(x, y);
    if (target >= 0 && atom.st.os_state[target] == 0) trigger_boot_os(target);
}

- (void)mouseDragged:(NSEvent *)e {
    if (!atom.dragging) return;
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    int x = (int)(p.x * 2.0), y = HEIGHT - (int)(p.y * 2.0);
    atom.st.file_x = x - atom.drag_off_x;
    atom.st.file_y = y - atom.drag_off_y;
    if (atom.st.file_x < 10) atom.st.file_x = 10;
    if (atom.st.file_x > WIDTH - 330) atom.st.file_x = WIDTH - 330;
    if (atom.st.file_y < 10) atom.st.file_y = 10;
    if (atom.st.file_y > HEIGHT - 350) atom.st.file_y = HEIGHT - 350;

    int cx = atom.st.file_x + 160, cy = atom.st.file_y + 118;
    atom.hover_os = get_os_pane_at(cx, cy);
}

- (void)mouseUp:(NSEvent *)e {
    if (!atom.dragging) return;
    atom.dragging = 0;
    int cx = atom.st.file_x + 160, cy = atom.st.file_y + 118;
    int drop_os = get_os_pane_at(cx, cy);
    const char* names[3] = {"MAC OS 9.2", "WINDOWS XP", "UBUNTU LINUX"};

    if (drop_os >= 0) {
        if (atom.st.os_state[drop_os] == 0) trigger_boot_os(drop_os);
        atom.st.file_owner = drop_os;
        snprintf(atom.status_msg, sizeof(atom.status_msg),
                 atom.st.zoom_mode ? "RENDERED EXPANDED NEWS PHOTO INSIDE %s WINDOW!" : "DROPPED LIVE NEWS PHOTO INTO %s (DOUBLE-CLICK OR [Z] TO EXPAND!)",
                 names[drop_os]);
        atom.chime_os = 10; atom.chime_frames = 6500;
    } else {
        snap_file_to_os(atom.st.file_owner);
        strcpy(atom.status_msg, "RETURNED TO HOST OS SPACE");
    }
    atom.hover_os = -1;
}

- (void)keyDown:(NSEvent *)e {
    if ([[e characters] length] == 0) return;
    unichar ch = [[e characters] characterAtIndex:0];
    if (atom.pmode) {
        if (ch == 27) { atom.pmode = 0; atom.plen = 0; }
        else if (ch == 127 || ch == 8) { if (atom.plen > 0) atom.plen--; }
        else if (ch == 13 || ch == 10) {
            atom.pbuf[atom.plen] = 0;
            char *copy = strdup(atom.pbuf);
            atom.pmode = 0; atom.plen = 0;
            dispatch_async(dispatch_get_global_queue(0, 0), ^{ run_local_ai(copy); free(copy); });
        } else if (ch >= 32 && ch < 127 && atom.plen < 120) atom.pbuf[atom.plen++] = (char)ch;
        return;
    }
    if (ch == 47) { atom.pmode = 1; atom.plen = 0; }
    else if (ch == 49) trigger_boot_os(0);
    else if (ch == 50) trigger_boot_os(1);
    else if (ch == 51) trigger_boot_os(2);
    else if (ch == 98 || ch == 66) { atom.st.boot_all_queue = 1; trigger_boot_os(0); }
    else if (ch == 119 || ch == 87) {
        dispatch_async(dispatch_get_global_queue(0, 0), ^{ fetch_live_news_image(1); });
    }
    else if (ch == 122 || ch == 90) {
        atom.st.zoom_mode = !atom.st.zoom_mode;
        const char* names[3] = {"MAC OS 9.2", "WINDOWS XP", "UBUNTU LINUX"};
        snprintf(atom.status_msg, sizeof(atom.status_msg),
                 atom.st.zoom_mode ? "EXPANDED LIVE NEWS PHOTO INSIDE %s WINDOW!" : "COLLAPSED TO FLOATING CARD",
                 names[atom.st.file_owner]);
        atom.chime_os = 10; atom.chime_frames = 5500;
    }
    else if (ch == 115 || ch == 83) save_multi_atom();
    else if (ch == 108 || ch == 76) load_multi_atom();
    else if (ch == 99 || ch == 67) {
        atom.st.os_state[0] = 0; atom.st.os_state[1] = 0; atom.st.os_state[2] = 0;
        atom.st.zoom_mode = 0;
        snap_file_to_os(0);
        strcpy(atom.status_msg, "ALL 3 OSES SHUT DOWN (PRESS [B] TO BOOT OR [L] TO RESTORE)");
    }
}

- (void)drawRect:(NSRect)d {
    double f0 = now_ms();
    atom.tick++;
    if (atom.tick % 15 == 1) atom.ram_mb = get_real_ram_mb();

    for (int k = 0; k < 3; k++) {
        if (atom.st.os_state[k] == 1) {
            atom.st.boot_timer[k]++;
            if (atom.st.boot_timer[k] >= 55) {
                atom.st.os_state[k] = 2;
                if (atom.st.boot_all_queue == 1 && k == 0) { atom.st.boot_all_queue = 2; trigger_boot_os(1); }
                else if (atom.st.boot_all_queue == 2 && k == 1) { atom.st.boot_all_queue = 0; trigger_boot_os(2); }
                else if (atom.st.os_state[0]==2 && atom.st.os_state[1]==2 && atom.st.os_state[2]==2) {
                    strcpy(atom.status_msg, "ALL 3 OSES ONLINE! DRAG NEWS PHOTO & DOUBLE-CLICK TO EXPAND IN WINDOW!");
                }
            }
        }
    }

    for (int i = 0; i < WIDTH * HEIGHT; i++) fb[i] = 0xFF06080D;

    render_macos9_bg(28, 28, PANE_W, PANE_H);
    stroke_rect(28, 28, PANE_W, PANE_H, (atom.dragging && atom.hover_os == 0) ? 6 : 3,
                (atom.dragging && atom.hover_os == 0) ? 0xFF00E5FF : 0xFF334155);

    render_winxp_bg(1132, 28, PANE_W, PANE_H);
    stroke_rect(1132, 28, PANE_W, PANE_H, (atom.dragging && atom.hover_os == 1) ? 6 : 3,
                (atom.dragging && atom.hover_os == 1) ? 0xFF00E5FF : 0xFF334155);

    render_ubuntu_bg(28, 676, PANE_W, PANE_H);
    stroke_rect(28, 676, PANE_W, PANE_H, (atom.dragging && atom.hover_os == 2) ? 6 : 3,
                (atom.dragging && atom.hover_os == 2) ? 0xFF00E5FF : 0xFF334155);

    render_control_bg(1132, 676, PANE_W, PANE_H);

    draw_live_news_viewer();

    if (atom.pmode || atom.thinking) {
        rect(28, HEIGHT - 64, WIDTH - 56, 52, atom.thinking ? 0xFF553300 : 0xFF002B36);
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(fb, WIDTH, HEIGHT, 8, WIDTH * 4, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);

    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext *nsCtx = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:YES];
    [NSGraphicsContext setCurrentContext:nsCtx];
    CGContextTranslateCTM(ctx, 0, HEIGHT);
    CGContextScaleCTM(ctx, 1.0, -1.0);
    render_all_hd_labels();
    [NSGraphicsContext restoreGraphicsState];

    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextDrawImage([[NSGraphicsContext currentContext] CGContext], CGRectMake(0, 0, LOG_W, LOG_H), img);
    CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs);

    atom.frame_ms = now_ms() - f0;
}
@end

int main(void) {
    fb = calloc(WIDTH * HEIGHT, 4);
    load_all_assets();
    atom.ram_mb = get_real_ram_mb();
    dispatch_async(dispatch_get_global_queue(0, 0), ^{ fetch_live_news_image(0); });

    AudioStreamBasicDescription f = {SR, kAudioFormatLinearPCM, kLinearPCMFormatFlagIsSignedInteger|kLinearPCMFormatFlagIsPacked, 2, 1, 2, 1, 16, 0};
    AudioQueueRef q; AudioQueueNewOutput(&f, audio_cb, NULL, NULL, NULL, 0, &q);
    for (int i = 0; i < 3; i++) { AudioQueueBufferRef b; AudioQueueAllocateBuffer(q, 2048, &b); audio_cb(NULL, q, b); }
    AudioQueueStart(q, NULL);

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(60, 60, LOG_W, LOG_H) styleMask:3 backing:2 defer:NO];
    [w setTitle:@"ZERO OS M2 - Live Web News Image & Cross-OS Window Expansion"];
    ZView *v = [[ZView alloc] initWithFrame:NSMakeRect(0, 0, LOG_W, LOG_H)];
    [w setContentView:v]; [w makeKeyAndOrderFront:nil]; [w makeFirstResponder:v];
    [NSApp activateIgnoringOtherApps:YES];
    [NSTimer scheduledTimerWithTimeInterval:1.0/60.0 repeats:YES block:^(NSTimer *t){ [v setNeedsDisplay:YES]; }];
    [NSApp run];
}
