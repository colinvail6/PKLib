#pragma once
/*
 * pixelkit.hpp - Kano Pixel Kit C++ library
 *
 * Mirrors pixelkit.py exactly. No Python dependency.
 *
 * Compile your program with:
 *   g++ -O2 -o myprog myprog.cpp -lm
 *
 * Usage:
 *   #include "pixelkit.hpp"
 *   PixelKit kit;
 *   kit.connect();
 *   kit.set_background({255, 0, 0});
 *   kit.render();
 */

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <functional>
#include "scroll_letters.hpp"
#include "scroll_numbers.hpp"
#include "scroll_symbols.hpp"

/* ---- Constants ----------------------------------------------------------- */

static const int PK_W      = 16;
static const int PK_H      = 8;
static const int PK_NPIX   = PK_W * PK_H;
static const int PAD_LEN   = 227;

/* ---- Types --------------------------------------------------------------- */

struct RGB { uint8_t r, g, b; };

/* ---- Helpers ------------------------------------------------------------- */

static RGB hsv_to_rgb(float h, float s, float v) {
    RGB c = {0, 0, 0};
    if (s == 0.0f) {
        uint8_t x = (uint8_t)(v * 255.0f);
        c.r = c.g = c.b = x;
        return c;
    }
    int   hi = (int)(h * 6.0f) % 6;
    float f  =  h * 6.0f - (int)(h * 6.0f);
    float p  = v * (1.0f - s);
    float q  = v * (1.0f - s * f);
    float t  = v * (1.0f - s * (1.0f - f));
    switch (hi) {
        case 0: c.r=v*255; c.g=t*255; c.b=p*255; break;
        case 1: c.r=q*255; c.g=v*255; c.b=p*255; break;
        case 2: c.r=p*255; c.g=v*255; c.b=t*255; break;
        case 3: c.r=p*255; c.g=q*255; c.b=v*255; break;
        case 4: c.r=t*255; c.g=p*255; c.b=v*255; break;
        case 5: c.r=v*255; c.g=p*255; c.b=q*255; break;
    }
    return c;
}

static const char _B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const uint8_t *src, int len, char *dst) {
    int j = 0;
    for (int i = 0; i < len; i += 3) {
        uint8_t  a   = src[i];
        uint8_t  b   = (i+1 < len) ? src[i+1] : 0;
        uint8_t  c   = (i+2 < len) ? src[i+2] : 0;
        uint32_t val = ((uint32_t)a<<16)|((uint32_t)b<<8)|c;
        dst[j++] = _B64[(val>>18)&0x3F];
        dst[j++] = _B64[(val>>12)&0x3F];
        dst[j++] = (i+1<len) ? _B64[(val>>6)&0x3F] : '=';
        dst[j++] = (i+2<len) ? _B64[ val    &0x3F] : '=';
    }
    dst[j] = '\0';
}

/* ---- PixelKit class ------------------------------------------------------ */

class PixelKit {
public:
    RGB  pixels[PK_NPIX];
    int  fd = -1;
    bool connected = false;

    PixelKit() { memset(pixels, 0, sizeof pixels); }
    ~PixelKit() { if (fd >= 0) ::close(fd); }

    /* ---- Callbacks — set before connect() -------------------------------- */

    std::function<void()>    on_joystick_up    = nullptr;
    std::function<void()>    on_joystick_down  = nullptr;
    std::function<void()>    on_joystick_left  = nullptr;
    std::function<void()>    on_joystick_right = nullptr;
    std::function<void()>    on_joystick_click = nullptr;
    std::function<void()>    on_button_a       = nullptr;
    std::function<void()>    on_button_b       = nullptr;
    std::function<void()>    on_button_reset   = nullptr;
    std::function<void(int)> on_dial           = nullptr;

    /*
     * connect() — mirrors Python connect() exactly:
     *   1. Open /dev/ttyS2 at 115200 baud, 8N1, XON/XOFF
     *   2. Sleep 0.5s
     *   3. send_break(5ms) + 'U' sync char
     *   4. Sleep 2s
     *   5. _stop_animation() x2 with 1s gap
     */
    bool connect(const char *port = "/dev/ttyS2") {
        fd = ::open(port, O_RDWR | O_NOCTTY);
        if (fd < 0) { perror("open"); return false; }

        /* Configure — must match Python SerialChannel settings */
        struct termios tty;
        memset(&tty, 0, sizeof tty);
        tcgetattr(fd, &tty);

        cfsetispeed(&tty, B115200);
        cfsetospeed(&tty, B115200);

        /* 8N1 */
        tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
        tty.c_cflag |=  CS8 | CLOCAL | CREAD;

        /* XON/XOFF software flow control — matches Python xonxoff=True */
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP |
                         INLCR  | IGNCR  | ICRNL  | IXANY);
        tty.c_iflag |=  IXON | IXOFF;

        /* Raw output */
        tty.c_oflag &= ~OPOST;

        /* Raw input, no echo */
        tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

        /* Non-blocking reads */
        tty.c_cc[VMIN]  = 0;
        tty.c_cc[VTIME] = 0;

        if (tcsetattr(fd, TCSANOW, &tty) < 0) {
            perror("tcsetattr"); return false;
        }
        tcflush(fd, TCIOFLUSH);

        /* Step 2: sleep 0.5s */
        usleep(500000);

        /* Step 3: send_break(5ms) + U */
        fprintf(stderr, "[pk] send_break\n");
        ioctl(fd, TIOCSBRK, 0);
        usleep(5000);
        ioctl(fd, TIOCCBRK, 0);
        write(fd, "U", 1);

        /* Step 4: sleep 2s */
        sleep(2);

        /* Step 5: stop animation x2 */
        fprintf(stderr, "[pk] stop animation\n");
        _stop_animation();
        sleep(1);
        _stop_animation();
        sleep(1);

        connected = true;
        fprintf(stderr, "[pk] connected\n");
        return true;
    }

    /* Set one pixel */
    void set_pixel(int x, int y, RGB c) {
        if (x >= 0 && x < PK_W && y >= 0 && y < PK_H)
            pixels[y * PK_W + x] = c;
    }

    /* Set one pixel by HSV */
    void set_pixel_hsv(int x, int y, float h, float s, float v) {
        set_pixel(x, y, hsv_to_rgb(h, s, v));
    }

    /* Fill entire display */
    void set_background(RGB c) {
        for (int i = 0; i < PK_NPIX; i++) pixels[i] = c;
    }

    /* Turn off all pixels */
    void clear() { set_background({0, 0, 0}); }

    /* ---- Text / scroll --------------------------------------------------- */

    /*
     * draw_letter() — draw a single character at pixel position (x, y).
     * Characters are 3 wide x 5 tall.
     */
    void draw_letter(int x, int y, char c, RGB col = {255,255,255}) {
        const uint8_t (*bmp)[3] = _get_glyph(c);
        if (!bmp) return;
        for (int row = 0; row < 5; row++)
            for (int col_i = 0; col_i < 3; col_i++)
                if (bmp[row][col_i])
                    set_pixel(x + col_i, y + row, col);
    }

    /*
     * scroll() — scroll a string across the display from right to left.
     * Matches Python scroll() behaviour.
     */
    void scroll(const char *text, RGB col = {255,255,255},
                RGB bg = {0,0,0}, int interval_ms = 100) {
        /* Build scroll buffer: 16 blank columns + characters + 16 blank */
        int   len    = strlen(text);
        /* Each char is 3 wide + 1 gap, prefix/suffix 16 blank columns */
        int   total  = 16 + len * 4 + 16;
        /* buf[row][col] */
        uint8_t **buf = new uint8_t*[5];
        for (int r = 0; r < 5; r++) {
            buf[r] = new uint8_t[total]();
        }

        /* Stamp each character into buffer */
        int xoff = 16;
        for (int ci = 0; ci < len; ci++) {
            const uint8_t (*bmp)[3] = _get_glyph(text[ci]);
            if (bmp) {
                for (int row = 0; row < 5; row++)
                    for (int col_i = 0; col_i < 3; col_i++)
                        buf[row][xoff + col_i] = bmp[row][col_i];
            }
            xoff += 4;   /* 3 wide + 1 gap */
        }

        /* Scroll */
        for (int offset = 0; offset < total; offset++) {
            set_background(bg);
            for (int x = 0; x < 16; x++) {
                int src = offset + x;
                if (src >= total) continue;
                for (int row = 0; row < 5; row++)
                    if (buf[row][src])
                        set_pixel(x, 1 + row, col);
            }
            render();
            usleep(interval_ms * 1000);
        }

        usleep(100000);
        clear();
        render();

        for (int r = 0; r < 5; r++) delete[] buf[r];
        delete[] buf;
    }

    /* Play a tone — non-blocking */
    void beep(int frequency, float duration_sec) {
        char msg[256];
        int ms  = (int)(duration_sec * 1000.0f);
        int len = snprintf(msg, sizeof msg,
            "{\"type\": \"rpc-request\", "
            "\"id\": \"b\", "
            "\"method\": \"play-tone\", "
            "\"params\": [{\"freq\": %d, \"duration\": %d}]}\r\n",
            frequency, ms);
        _send(msg, len);
    }

    /* Poll MCU for events and fire callbacks */
    void check_controls() {
        char buf[2048];
        int  total = 0;
        int  n;

        /* Read all available bytes */
        while ((n = read(fd, buf + total, sizeof buf - total - 1)) > 0)
            total += n;

        if (total <= 0) return;
        buf[total] = '\0';

        char *p = buf;
        while ((p = strchr(p, '{')) != nullptr) {
            if (strstr(p, "button-down") != nullptr) {
                if      (strstr(p, "js-up"))     { if (on_joystick_up)    on_joystick_up(); }
                else if (strstr(p, "js-down"))   { if (on_joystick_down)  on_joystick_down(); }
                else if (strstr(p, "js-left"))   { if (on_joystick_left)  on_joystick_left(); }
                else if (strstr(p, "js-right"))  { if (on_joystick_right) on_joystick_right(); }
                else if (strstr(p, "js-click"))  { if (on_joystick_click) on_joystick_click(); }
                else if (strstr(p, "btn-A"))     { if (on_button_a)       on_button_a(); }
                else if (strstr(p, "btn-B"))     { if (on_button_b)       on_button_b(); }
                else if (strstr(p, "btn-reset")) { if (on_button_reset)   on_button_reset(); }
            } else if (strstr(p, "mode-change") != nullptr) {
                int val = 0;
                if      (strstr(p, "offline-1")) val = 0;
                else if (strstr(p, "offline-2")) val = 1;
                else if (strstr(p, "online-p1")) val = 2;
                else if (strstr(p, "online-p2")) val = 3;
                else if (strstr(p, "online-p3")) val = 4;
                if (on_dial) on_dial(val);
            }
            p++;
        }
    }

    /* Expose hsv_to_rgb as static member */
    static RGB hsv(float h, float s, float v) {
        return hsv_to_rgb(h, s, v);
    }
    void render() {
        if (!connected) return;

        /* Pack RGB565 big-endian */
        uint8_t raw[PK_NPIX * 2];
        for (int i = 0; i < PK_NPIX; i++) {
            uint16_t v = ((pixels[i].r & 0xF8) << 8)
                       | ((pixels[i].g & 0xFC) << 3)
                       |  (pixels[i].b >> 3);
            raw[i*2  ] = (v >> 8) & 0xFF;
            raw[i*2+1] =  v       & 0xFF;
        }

        /* Base64 */
        char b64[512];
        b64_encode(raw, PK_NPIX * 2, b64);

        /* JSON — spaces after colon and comma, matches Python json.dumps() */
        char msg[1024];
        int  len = snprintf(msg, sizeof msg,
            "{\"type\": \"rpc-request\", "
            "\"id\": \"r\", "
            "\"method\": \"grid-bmp\", "
            "\"params\": [{\"map\": \"%s\"}]}\r\n",
            b64);

        _send(msg, len);
    }

private:

    /* Returns 5x3 bitmap for any printable character */
    const uint8_t (*_get_glyph(char c))[3] {
        const uint8_t (*bmp)[3] = nullptr;
        bmp = get_letter(c);
        if (bmp) return bmp;
        bmp = get_number(c);
        if (bmp) return bmp;
        bmp = get_symbol(c);
        return bmp;
    }

    void _stop_animation() {
        const char msg[] =
            "{\"type\": \"rpc-request\", "
            "\"id\": \"s\", "
            "\"method\": \"start-anim-control\", "
            "\"params\": [{\"blackout-incr\": 8}]}\r\n";
        _send(msg, sizeof msg - 1);
    }

    /* Send with null padding to multiple of PAD_LEN */
    void _send(const char *msg, int len) {
        int padded = ((len / PAD_LEN) + 1) * PAD_LEN;
        char *buf  = (char *)calloc(padded, 1);
        memcpy(buf, msg, len);
        ssize_t n  = write(fd, buf, padded);
        if (n < 0) perror("write");
        free(buf);
    }
};
