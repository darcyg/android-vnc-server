/*
  droid vnc server - Android VNC server
  Copyright (C) 2009 Jose Pereira <onaips@gmail.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 3 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "droidvncserver.hpp"
#include "rotation_watcher.hpp"
#include "update_screen.hpp"
#include "png.h"

#include "rfb/keysym.h"
#include "libvncserver/scale.h"
#include "Minicap.hpp"
#include "JpgEncoder.hpp"
#include <linux/fb.h>
#include <linux/input.h>

#define ROT_0 (1 << 0)
#define ROT_90 (1 << 1)
#define ROT_180 (1 << 2)
#define ROT_270 (1 << 3)
#define ROT_ALL (ROT_0 | ROT_90 | ROT_180 | ROT_270)

#define PID_FILE "/data/local/tmp/vnc.pid"

#define SCREENSHOT_PNG_RGBA_WHEN_CONVENIENT 0
#define SCREENSHOT_JPG_QUALITY 80
#define SCREENSHOT_SIGNAL_FILE "/data/local/tmp/screen.png"

#define test_bit(bit, array)    (array[bit/8] & (1<<(bit%8)))
enum {
  /* The input device is a touchscreen or a touchpad (either single-touch or multi-touch). */
  INPUT_DEVICE_CLASS_TOUCH         = 0x00000004,
  /* The input device is a multi-touch touchscreen. */
  INPUT_DEVICE_CLASS_TOUCH_MT      = 0x00000010,
  /* The input device is a multi-touch touchscreen and needs MT_SYNC. */
  INPUT_DEVICE_CLASS_TOUCH_MT_SYNC = 0x00000200
};

// Configurables
static int serverPort = 5901; // Android already has 5900 bound natively in some devices.
static char serverPassword[256] = "";
static char serverPasswordFile[1024] = "";
static int allowedRotation = ROT_ALL;
static bool forcedRotation = false; // true if allowedRotation != ROT_ALL
static bool rotate180 = false;
static uint16_t scaling = 100;
static bool skipFrames = false;
static int desiredBpp = -1;
static char *screenshotFile = NULL;
static bool screenshotFast = false;
static bool screenshotSkipFrame = false;

// Screen info
static fb_var_screeninfo screenInfo;
static int screenWidth = 0, screenHeight = 0;
static int screenRotation; // Current screen rotation
static int imageRotation; // Required frame rotation

// shared VNC buffers and screen
Minicap::Frame frame;
rfbScreenInfoPtr vncscr;
// unsigned char *cmpbuf;
unsigned char *vncbuf;

// Reverse connection
static char *rhost = NULL;
static int rport = 5500;

// Minicap object
static Minicap *minicap = NULL;

//key and touch
static int kbdfd = -1;
static int touchfd = -1;
static int xmin, xmax;
static int ymin, ymax;
static char KBD_DEVICE[256] = "/dev/input/event3";
static char TOUCH_DEVICE[256] = "/dev/input/event5";
static bool mTouchDown = false;
static int mCursorX = -1;
static int mCursorY = -1;
static int mTrackId = 1;
//device flags indicate single touch or multiple touch
static uint32_t g_device_flags;

// TODO: cleanup
static uint32_t idle = 0;
static uint32_t standby = 1;

void print(int logPriority, FILE* stream, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stream, format, args);
    if (logPriority != -1) __android_log_vprint(logPriority, LOG_TAG, format, args);
    va_end(args);
    fprintf(stream, "\n");
}

static void onClientGone(rfbClientPtr cl)
{
    LOGD("onClientGone: Client disconnected");
}

static rfbNewClientAction onClientConnect(rfbClientPtr cl)
{
    LOGD("onClientConnect: New client connection");
    if (scaling != 100) {
        unsigned int scaledWidth = (unsigned int) (vncscr->width * scaling / 100.0 + 0.5);
        unsigned int scaledHeight = (unsigned int) (vncscr->height * scaling / 100.0 + 0.5);
        rfbScalingSetup(cl, scaledWidth, scaledHeight);
    }

    // cl->enableSupportedEncodings = TRUE;
    cl->clientGoneHook = (ClientGoneHookPtr) onClientGone;
    return RFB_CLIENT_ACCEPT;
}

static void MakeRichCursor(rfbScreenInfoPtr rfbScreen, int bpp)
{
  int i,j,w=32,h=32;
  rfbCursorPtr c;
  char bitmap[]=
    "                                "
    "                                "
    "                                "
    "                                "
    "                                "
    "                                "
    "           X                    "
    "           XX                   "
    "           XXX                  "
    "           XXXX                 "
    "           XXXXX                "
    "           XXXXXX               "
    "           XXXXXXX              "
    "           XXXXXXXX             "
    "           XXXXXXXXX            "
    "           XXXXXXXXXX           "
    "           XXXXXX               "
    "           XXXXXXX              "
    "           XXX XXXX             "
    "           XX  XXXX             "
    "           X    XXXX            "
    "                XXXX            "
    "                 XXXX           "
    "                 XXXX           "
    "                  XXX           "
    "                   XX           "
    "                                "
    "                                "
    "                                "
    "                                "
    "                                "
    "                                ";
  c=rfbScreen->cursor = rfbMakeXCursor(w,h,bitmap,NULL);
  c->xhot = 7; c->yhot = 12;
}


static void injectInputEvent(int fd, int type, int code, int value) {
    if (fd != -1) {
        LOGD("injectInputEvent %04x %04x %08x", type, code, value);
        struct input_event ev = { 0 };
        gettimeofday(&ev.time, 0);
        ev.type = type;
        ev.code = code;
        ev.value = value;
        if (write(fd, &ev, sizeof(ev)) < 0)
            LOGD("write event failed, %s\n", strerror(errno));
    }
}

//multi touch
static void injectTouchEventMultiTouch(int x, int y, bool down) {
    bool hasEvent = false;
    LOGD("----receiveTouchEvent x:%d y:%d down:%d", x, y, down?1:0);
    if (down) {
        if (down != mTouchDown) {
            injectInputEvent(touchfd, EV_ABS, ABS_MT_TRACKING_ID,
                    mTrackId++);
        }
        bool hasMoved =  false;
        if (x != mCursorX) {
            mCursorX = x;
            injectInputEvent(touchfd, EV_ABS, ABS_MT_POSITION_X, x);
            hasEvent = true;
            hasMoved = true;
        }
        if (y != mCursorY) {
            mCursorY = y;
            injectInputEvent(touchfd, EV_ABS, ABS_MT_POSITION_Y, y);
            hasEvent = true;
            hasMoved = true;
        }
        if (hasMoved) {
            injectInputEvent(touchfd, EV_ABS, ABS_MT_PRESSURE, 127);
            injectInputEvent(touchfd, EV_ABS, ABS_MT_TOUCH_MAJOR, 127);
            injectInputEvent(touchfd, EV_ABS, ABS_MT_WIDTH_MAJOR, 4);
        }
        if (down != mTouchDown) {
            if (g_device_flags & INPUT_DEVICE_CLASS_TOUCH_MT_SYNC) {
                injectInputEvent(touchfd, EV_SYN, SYN_MT_REPORT, 0);
            }
            injectInputEvent(touchfd, EV_KEY, BTN_TOUCH, 1);
        }
    } else {
        if (down != mTouchDown) {
            injectInputEvent(touchfd, EV_ABS, ABS_MT_TRACKING_ID, -1);
            if (g_device_flags & INPUT_DEVICE_CLASS_TOUCH_MT_SYNC) {
                injectInputEvent(touchfd, EV_SYN, SYN_MT_REPORT, 0);
            }
            mCursorX = mCursorY = -1;
            injectInputEvent(touchfd, EV_KEY, BTN_TOUCH, 0);
        }
    }
    if (down != mTouchDown) {
        mTouchDown = down;
        hasEvent = true;
    }
    if (hasEvent) {
        injectInputEvent(touchfd, EV_SYN, SYN_REPORT, 0);
    }
}

//single touch
static void injectTouchEventSingleTouch(int x, int y, bool down) {
    bool hasEvent = false;
    if (down) {
        if (x != mCursorX) {
            mCursorX = x;
            injectInputEvent(touchfd, EV_ABS, ABS_X, x);
            hasEvent = true;
        }
        if (y != mCursorY) {
            mCursorY = y;
            injectInputEvent(touchfd, EV_ABS, ABS_Y, y);
            hasEvent = true;
        }
        if (down != mTouchDown)
            injectInputEvent(touchfd, EV_KEY, BTN_TOUCH, 1);
    } else {
        if (down != mTouchDown) {
            mCursorX = mCursorY = -1;
            injectInputEvent(touchfd, EV_KEY, BTN_TOUCH, 0);
        }
    }
    if (down != mTouchDown) {
        mTouchDown = down;
        hasEvent = true;
    }
    if (hasEvent) {
        injectInputEvent(touchfd, EV_SYN, SYN_REPORT, 0);
    }
}

void injectKeyEvent(uint16_t code, uint16_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time,0);
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;
    if(write(kbdfd, &ev, sizeof(ev)) < 0)
    {
        LOGD("write event failed, %s\n", strerror(errno));
    }

    LOGD("injectKey (%d, %d)\n", code , value);
}

static int keysym2scancode(rfbBool down, rfbKeySym key, rfbClientPtr cl)
{
    int scancode = 0;

    int code = (int)key;
    if (code>='0' && code<='9') {
        scancode = (code & 0xF) - 1;
        if (scancode<0) scancode += 10;
        scancode += KEY_1;
    } else if (code>=0xFF50 && code<=0xFF58) {
        static const uint16_t map[] =
             {  KEY_HOME, KEY_LEFT, KEY_UP, KEY_RIGHT, KEY_DOWN,
                KEY_END, 0 };
        scancode = map[code & 0xF];
    } else if (code>=0xFFE1 && code<=0xFFEE) {
        static const uint16_t map[] =
             {  KEY_LEFTSHIFT, KEY_LEFTSHIFT,
                KEY_COMPOSE, KEY_COMPOSE,
                KEY_LEFTSHIFT, KEY_LEFTSHIFT,
                0,0,
                KEY_LEFTALT, KEY_RIGHTALT,
                0, 0, 0, 0 };
        scancode = map[code & 0xF];
    } else if ((code>='A' && code<='Z') || (code>='a' && code<='z')) {
        static const uint16_t map[] = {
                KEY_A, KEY_B, KEY_C, KEY_D, KEY_E,
                KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
                KEY_K, KEY_L, KEY_M, KEY_N, KEY_O,
                KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
                KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z };
        scancode = map[(code & 0x5F) - 'A'];
    } else {
        switch (code) {
            case 0x0020:    scancode = KEY_SPACE;       break;
            case 0x002C:    scancode = KEY_COMMA;       break;
            case 0x003C:    scancode = KEY_COMMA;       break;
            case 0x002E:    scancode = KEY_DOT;         break;
            case 0x003E:    scancode = KEY_DOT;         break;
            case 0x002F:    scancode = KEY_SLASH;       break;
            case 0x003F:    scancode = KEY_SLASH;       break;
            case 0x0032:    scancode = KEY_EMAIL;       break;
            case 0x0040:    scancode = KEY_EMAIL;       break;
            case 0xFF08:    scancode = KEY_BACKSPACE;   break;
            case 0xFF1B:    scancode = KEY_BACK;        break;
            case 0xFF09:    scancode = KEY_TAB;         break;
            case 0xFF0D:    scancode = KEY_ENTER;       break;
            case 0xFFBE:    scancode = KEY_F1;        break; // F1
            case 0xFFBF:    scancode = KEY_F2;         break; // F2
            case 0xFFC0:    scancode = KEY_F3;        break; // F3
            case 0xFFC5:    scancode = KEY_F4;       break; // F8
            case 0xFFC8:    rfbShutdownServer(cl->screen,TRUE);       break; // F11
        }
    }

    return scancode;
}

static void onKeyEvent(rfbBool down, rfbKeySym key, rfbClientPtr cl) {
    int scancode;

    LOGD("Got keysym: %04x (down=%d)\n", (unsigned int)key, (int)down);

    if ((scancode = keysym2scancode(down, key, cl)))
    {
        injectKeyEvent(scancode, down);
    }
}

static void onMultiplePointerEvent(int buttonMask, int x, int y, rfbClientPtr cl) {
    int tx = x;
    int ty = y;
    switch (screenRotation) {
        case 90:
            tx = screenWidth - y;
            ty = x;
            break;
        case 270:
            tx = y;
            ty = screenHeight - x;
            break;
        default:
            break;
    }
    tx = tx*xmax/screenWidth;
    ty = ty*ymax/screenHeight;
    injectTouchEventMultiTouch(tx, ty, (buttonMask&1)!=0);
}

static void onSinglePointerEvent(int buttonMask, int x, int y, rfbClientPtr cl) {
    int tx = x;
    int ty = y;
    switch (screenRotation) {
        case 90:
            tx = screenWidth - y;
            ty = x;
            break;
        case 270:
            tx = y;
            ty = screenHeight - x;
            break;
        default:
            break;
    }
    tx = tx*xmax/screenWidth;
    ty = ty*ymax/screenHeight;
    injectTouchEventSingleTouch(tx, ty, (buttonMask&1)!=0);
}

//remove trailing spaces
static char *rtrim(char *str) {
    if (str == NULL || *str == '\0')
    {
        return str;
    }
    int len = strlen(str);
    char *p = str + len - 1;
    while (p >= str && isspace(*p))
    {
        *p = '\0'; --p;
    }
    return str;
} 

static void findKeyAndTouchDevice() {
    FILE *fp = popen("getevent -ilq", "r");
    char buf[40961];
    if (fp == NULL) {
        FATAL("error could not getevent");
        return;
    }
    fread(buf, sizeof(char), 40960, fp);
    pclose(fp);
    char *p = strstr(buf, "add device");
    bool keyFind = false;
    bool touchFind = false;
    while(p) {
        char *nextp = strstr(p+10, "add device");
        if (nextp!= NULL) {
            *nextp = '\0';
        }
        if (strstr(p,"ABS_MT_POSITION_X ")!=NULL) {
            char *st = strchr(p, '/');
            if (st!=NULL) {
                char *ed = strchr(p, '\n');
                if (ed == NULL) {
                    continue;
                }
                touchFind = true;
                memcpy(TOUCH_DEVICE, st, sizeof(char)*(ed-st));
                TOUCH_DEVICE[ed-st+1]='\0';
                rtrim(TOUCH_DEVICE);
            }
        }
        if (strstr(p, "KEY_HOME")!=NULL) {
            char *st = strchr(p, '/');
            if (st!=NULL) {
                char *ed = strchr(p, '\n');
                if (ed == NULL) {
                    continue;
                }
                keyFind = true;
                memcpy(KBD_DEVICE, st, sizeof(char)*(ed-st));
                KBD_DEVICE[ed-st+1]='\0';
                rtrim(KBD_DEVICE);
            }
        }
        if (touchFind && keyFind ) {
            break;
        }
        if (nextp!= NULL) {
            *nextp = 'a';
        }
        p = nextp;
    }
}

static void init_kbd() {
    if((kbdfd = open(KBD_DEVICE, O_RDWR)) == -1) {
        LOGD("cannot open kbd device %s\n", KBD_DEVICE);
        exit(EXIT_FAILURE);
    }
}

static uint32_t figure_out_events_device_reports(int fd) {

  uint32_t device_classes = 0;

  uint8_t key_bitmask[(KEY_MAX + 1) / 8 + !!((KEY_MAX + 1) % 8)];
  uint8_t abs_bitmask[(ABS_MAX + 1) / 8 + !!((ABS_MAX + 1) % 8)];

  memset(key_bitmask, 0, sizeof(key_bitmask));
  memset(abs_bitmask, 0, sizeof(abs_bitmask));

  ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bitmask)), key_bitmask);
  ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bitmask)), abs_bitmask);

  // See if this is a touch pad.
  // Is this a new modern multi-touch driver?
  if (test_bit(ABS_MT_POSITION_X, abs_bitmask)
      && test_bit(ABS_MT_POSITION_Y, abs_bitmask)) {
    // Some joysticks such as the PS3 controller report axes that conflict
    // with the ABS_MT range.  Try to confirm that the device really is
    // a touch screen.
    // Mozilla Bug 741038 - support GB touchscreen drivers
    //if (test_bit(BTN_TOUCH, device->keyBitmask) || !haveGamepadButtons) {
    device_classes |= INPUT_DEVICE_CLASS_TOUCH | INPUT_DEVICE_CLASS_TOUCH_MT;
    char device_name[80];

    if(ioctl(fd, EVIOCGNAME(sizeof(device_name) - 1), &device_name) < 1) {
      //fprintf(stderr, "could not get device name for %s, %s\n", device, strerror(errno));
      device_name[0] = '\0';
    }

    // some touchscreen devices expect MT_SYN events to be sent after every
    // touch
    if(strcmp(device_name, "atmel-touchscreen") == 0 ||
       strcmp(device_name, "nvodm_touch") == 0 ||
       strcmp(device_name, "elan-touchscreen") == 0 ||
       strcmp(device_name, "ft5x06_ts") == 0) {
      device_classes |= INPUT_DEVICE_CLASS_TOUCH_MT_SYNC;
    }

  // Is this an old style single-touch driver?
  } else if ((test_bit(BTN_TOUCH, key_bitmask)
              && test_bit(ABS_X, abs_bitmask)
              && test_bit(ABS_Y, abs_bitmask))) {
    device_classes |= INPUT_DEVICE_CLASS_TOUCH;
  }

  return device_classes;
}

static void cleanup_kbd() {
    if(kbdfd != -1) {
        close(kbdfd);
        kbdfd = -1;
    }
}

static void init_touch() {
    struct input_absinfo info;
    if((touchfd = open(TOUCH_DEVICE, O_RDWR)) == -1) {
            LOGD("cannot open touch device %s\n", TOUCH_DEVICE);
            exit(EXIT_FAILURE);
    }
    // Get the Range of X and Y
    if(ioctl(touchfd, EVIOCGABS(ABS_X), &info)) {
        LOGD("cannot get ABS_X info, %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    xmin = info.minimum;
    xmax = info.maximum;
    if (xmax <=0 ) {
        if(ioctl(touchfd, EVIOCGABS(ABS_MT_POSITION_X), &info)) {
            LOGD("cannot get ABS_MT_POSITION_X info, %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        xmin = info.minimum;
        xmax = info.maximum;
    }
    if(ioctl(touchfd, EVIOCGABS(ABS_Y), &info)) {
        LOGD("cannot get ABS_Y, %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    ymin = info.minimum;
    ymax = info.maximum;
    if (ymax <=0) {
        if(ioctl(touchfd, EVIOCGABS(ABS_MT_POSITION_Y), &info)) {
            LOGD("cannot get ABS_MT_POSITION_Y, %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        ymin = info.minimum;
        ymax = info.maximum;
    }
    LOGD("xmin:%d,xmax:%d,ymin:%d,ymax:%d", xmin,xmax,ymin,ymax);
    g_device_flags = figure_out_events_device_reports(touchfd);
}

static void cleanup_touch() {
    if(touchfd != -1) {
        close(touchfd);
        touchfd = -1;
    }
}

static void reinitVncServer(int width, int height, int stride, int bpp) {
    int targetWidth, targetHeight;
    if (forcedRotation && (imageRotation == 90 || imageRotation == 270)) {
        targetWidth = height;
        targetHeight = width;
    } else {
        targetWidth = width;
        targetHeight= height;
    }

    if (vncscr->width != targetWidth || vncscr->height != targetHeight) {
        unsigned int scaledWidth = (unsigned int) (targetWidth * scaling / 100.0 + 0.5);
        unsigned int scaledHeight = (unsigned int) (targetHeight * scaling / 100.0 + 0.5);
        rfbClientIteratorPtr iterator = rfbGetClientIterator(vncscr);
        rfbClientPtr cl;
        while ((cl = rfbClientIteratorNext(iterator)) != NULL) {
            rfbScalingSetup(cl, scaledWidth, scaledHeight);
        }

        vncscr->width = targetWidth;
        vncscr->height = targetHeight;
        vncscr->paddedWidthInBytes = targetWidth * bpp;
    }
}

static void initVncServer(int argc, char **argv, unsigned int width, unsigned int height, unsigned int stride, unsigned int bpp) {
    LOGD("initVncServer: bpp: %d, width: %d, height: %d, stride: %d", bpp, width, height, stride);

    if ((vncbuf = (unsigned char *) malloc(width * height * bpp)) == NULL)
        FATAL("Could not create vnc buffer");

    // if ((cmpbuf = (unsigned char *) malloc(width * height * bpp)) == NULL)
    //     FATAL("Could not create compare buffer");

    int targetWidth, targetHeight;
    if (forcedRotation && (imageRotation == 90 || imageRotation == 270)) {
        targetWidth = height;
        targetHeight = width;
            vncscr = rfbGetScreen(&argc, argv, height, width, 8, 3, bpp);
    } else {
        targetWidth = width;
        targetHeight = height;
    }
    vncscr = rfbGetScreen(&argc, argv, targetWidth, targetHeight, 8, 3, bpp);

    vncscr->bitsPerPixel = bpp * 8;
    vncscr->deferUpdateTime = 5; // 5ms
    vncscr->port = serverPort;
    vncscr->desktopName = (char *) "Android";
    vncscr->frameBuffer =(char *) vncbuf;
    vncscr->alwaysShared = TRUE;
    vncscr->kbdAddEvent = onKeyEvent;
    if (g_device_flags & INPUT_DEVICE_CLASS_TOUCH_MT) {
        vncscr->ptrAddEvent = onMultiplePointerEvent;
    }
    else if(g_device_flags & INPUT_DEVICE_CLASS_TOUCH) {
        vncscr->ptrAddEvent = onSinglePointerEvent;
    }
    vncscr->httpDir = (char *) "webclients/";
    vncscr->newClientHook = (rfbNewClientHookPtr) onClientConnect;

    vncscr->serverFormat.trueColour = TRUE;
    //vncscr->handleEventsEagerly = TRUE;
    vncscr->httpEnableProxyConnect =TRUE;
    vncscr->serverFormat.bitsPerPixel = bpp * 8;

    if (strcmp(serverPasswordFile, "") != 0) {
        LOGD("initVncServer: Using encrypted password file");
        vncscr->authPasswdData = serverPasswordFile;
    }
    else if (strcmp(serverPassword, "") != 0) {
        LOGD("initVncServer: Using plain text password");
        char **passwords = (char **)malloc(2 * sizeof(char **));
        passwords[0] = serverPassword;
        passwords[1] = NULL;
        vncscr->authPasswdData = passwords;
        vncscr->passwordCheck = rfbCheckPasswordByList;
    }

    vncscr->sslcertfile = (char *) "self.pem";
    MakeRichCursor(vncscr, bpp);

    rfbInitServer(vncscr);
}

const char *getImageFormatName() {
    switch (frame.format) {
    case Minicap::FORMAT_NONE:
        return "None";
        break;
    case Minicap::FORMAT_CUSTOM:
        return "Custom";
        break;
    case Minicap::FORMAT_TRANSLUCENT:
        return "Translucent";
        break;
    case Minicap::FORMAT_TRANSPARENT:
        return "Transparent";
        break;
    case Minicap::FORMAT_OPAQUE:
        return "Opaque";
        break;
    case Minicap::FORMAT_RGBA_8888:
        return "RGBA 8888";
        break;
    case Minicap::FORMAT_RGBX_8888:
        return "RGBX 8888";
        break;
    case Minicap::FORMAT_RGB_888:
        return "RGB 888";
        break;
    case Minicap::FORMAT_RGB_565:
        return "RGB 565";
        break;
    case Minicap::FORMAT_BGRA_8888:
        return "BGRA 8888";
        break;
    case Minicap::FORMAT_RGBA_5551:
        return "RGBA 5551";
        break;
    case Minicap::FORMAT_RGBA_4444:
        return "RGBA 4444";
        break;
    case Minicap::FORMAT_UNKNOWN:
        return "Unknown";
        break;
    default:
        FATAL("Unknown image format: %d", frame.format);
    }
    return NULL;
}

void quitSignal(int signum) {
    cleanup(0);
}

void cleanup(int exitCode) {
    bool isServer = screenshotFile == NULL;

    LOGD("Cleaning up...");

    // Deleting the server PID file could be added here, but that would cause
    // unnecessary activity on the flash memory. Leaving it is fine and would not
    // affect screenshot signalling behavior as the kill() call will fail.
    // if (isServer) {
    //     deleteServerPid();
    // }

    stop_rotation_watcher();

    if (minicap) {
        if (isServer) {
            // Server hasn't been initialized if minicap isn't
            // Don't disconnect clients here else a reconnect can happen before exit
            rfbShutdownServer(vncscr, FALSE);
        }
        minicap->setFrameAvailableListener(NULL);
        minicap_free(minicap);
    }

    free(vncbuf);
    // free(cmpbuf);
    cleanup_kbd();
    cleanup_touch();

    exit(exitCode);
}

static int extractHostPort(char **destHost, int *destPort, char *str)
{
    int len = strlen(str);
    char *p;

    /* copy in to host */
    char *rhost = *destHost = (char *) malloc(len + 1);
    if (!rhost) {
        LOGE("extractHostPort: could not malloc string %d", len);
        return 1;
    }
    strncpy(rhost, str, len);
    rhost[len] = '\0';

    /* extract port, if any */
    if ((p = strrchr(rhost, ':')) != NULL) {
        int port = atoi(p+1);
        if (port < 0) {
            port = -port;
        } else if (rport < 20) {
            port = 5500 + rport;
        }
        *p = '\0';
        *destPort = port;
    }
    return 0;
}

static int try_get_dumpsys_display_info(int *width, int *height) {
    FILE *f;
    char buf[4096], *p = NULL, *q;
    int w, h;

    if ((f = popen("dumpsys window", "r")) == NULL) {
        LOGE("Could not run dumpsys");
        return 1;
    }

    while (fgets(buf, 4095, f) != NULL) {
        p = strstr(buf, " init=");
        if (p) break;
    }

    if (!p) {
        LOGE("Unrecognized dumpsys output");
        return 1;
    }
    q = strstr(p, "x");
    if (!q) {
        LOGE("Unrecognized dumpsys output (2)");
        return 1;
    }

    p += 6;
    *q = 0;
    w = atoi(p);

    p = q + 1;
    q = strstr(p, " ");
    *q = 0;
    h = atoi(p);

    pclose(f);

    if (w <= 0 || h <= 0) {
        LOGE("Received invalid values from dumpsys");
        return 1;
    }

    *width = w;
    *height = h;

    return 0;
}

static int try_get_framebuffer_display_info(int *width, int *height) {
    int fd = open("/dev/graphics/fb0", O_RDONLY);
    if (fd < 0) {
        LOGE("Cannot open /dev/graphics/fb0");
        return -1;
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, &screenInfo) < 0) {
        close(fd);
        LOGE("Cannot get FBIOGET_VSCREENINFO of /dev/graphics/fb0");
        return -1;
    }
    *width = screenInfo.xres;
    *height = screenInfo.yres;
    close(fd);
    return 0;
}

class FrameWaiter: public Minicap::FrameAvailableListener {
public:
    FrameWaiter()
        : mTimeout(std::chrono::milliseconds(100)),
          mPendingFrames(0),
          mStopped(false) {
    }

    int waitForFrame() {
        std::unique_lock<std::mutex> lock(mMutex);
        while (!mStopped) {
            if (mCondition.wait_for(lock, mTimeout, [this]{return mPendingFrames > 0;})) {
                return mPendingFrames--;
            }
        }

        return 0;
    }

    void
    reportExtraConsumption(int count) {
        std::unique_lock<std::mutex> lock(mMutex);
        mPendingFrames -= count;
    }

    void
    onFrameAvailable() {
        std::unique_lock<std::mutex> lock(mMutex);
        mPendingFrames += 1;
        mCondition.notify_one();
    }

    int
    getPendingFrames() {
        std::unique_lock<std::mutex> lock(mMutex);
        if (!mPendingFrames) return 0;
        return mPendingFrames--;
    }

    void
    stop() {
        mStopped = true;
    }

    bool
    isStopped() {
        return mStopped;
    }

private:
    std::mutex mMutex;
    std::condition_variable mCondition;
    std::chrono::milliseconds mTimeout;
    int mPendingFrames;
    bool mStopped;
};

static void resetMinicapOrientation() {
    Minicap::DisplayInfo desiredInfo, realInfo;
    realInfo.width = desiredInfo.width = screenWidth;
    realInfo.height = desiredInfo.height = screenHeight;
    desiredInfo.orientation = screenRotation / 90;

    minicap->setRealInfo(realInfo);

    if (minicap->setDesiredInfo(desiredInfo) != 0)
        FATAL("Could not set desired display info");

    if (minicap->applyConfigChanges() != 0)
        FATAL("Could not apply config changes");
}

static unsigned int getImageRotation() {
    unsigned int s = screenRotation / 90, rot;
    if ((allowedRotation & (1 << s)) != 0) {
        rot = 0;
    } else if ((allowedRotation & (1 << ((s + 2) & 3))) != 0) {
        rot = 180;
    } else if ((allowedRotation & (1 << ((s + 1) & 3))) != 0) {
        rot = 270;
    } else { // ((allowedRotation & (1 << ((s + 3) & 3))) != 0)
        rot = 90;
    }
    return rotate180 ? (rot + 180) % 360 : rot;
}

static void printUsage(char **argv)
{
    P("\nUsage: %s [options]\n\n"
      "Server options:\n"
      "  -p <password>\t\t\t Password to access server\n"
      "  -e <encrypted password file>\t Use encrypted password file\n"
      "  -P <port>\t\t\t Server port\n"
      "  -R <host:port>\t\t Host for reverse connection\n\n"
      "Display options:\n"
      "  -d <width> <height>\t\t Specify screen dimensions\n"
      "  -r <rotation>\t\t\t Force screen rotation (degrees) (0, 90, 180, 270)\n"
      "  -o <orientation>\t\t Force screen orientation (landscape, portrait)\n"
      "  -z\t\t\t\t Rotate display another 180 degrees (for ZTE compatibility)\n"
      "  -s <scale>\t\t\t Scale percentage (0-100)\n"
      "  -b <bpp>\t\t\t Screen bytes per pixel (1, 2, 4, 8)\n"
      "  -f\t\t\t\t Enable frame skipping\n\n"
      "Other options:\n"
      "  -S <filename>\t\t\t Write JPEG or PNG screenshot to file and quit\n"
      "  -U \t\t\t Try to take screenshot using existing VNC server\n"
      "     \t\t\t (Saves to " SCREENSHOT_SIGNAL_FILE ")\n"
      "  -X \t\t\t Skip first frame when saving screenshot (for some Motorola devices)\n"
      "  -v\t\t\t\t Output version\n"
      "  -h\t\t\t\t Print this help\n", argv[0]);
}

static void writeScreenToFile(char *filename, int screenBpp) {
    int targetWidth, targetHeight;
    FILE *f;

    if (forcedRotation && (imageRotation == 90 || imageRotation == 270)) {
        targetWidth = frame.height;
        targetHeight = frame.width;
    } else {
        targetWidth = frame.width;
        targetHeight = frame.height;
    }

    if ((f = fopen(filename, "w+")) == NULL)
        FATAL("Could not open screenshot file");

    if (strstr(filename, ".png") != NULL) {
        png_structp png_ptr;
        png_infop info_ptr;
        png_byte **row_pointers;
        png_byte *row;

        // libpng with type PNG_COLOR_TYPE_RGB must have depth >= 8
        int bitDepth = 8;

        if ((png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL)) == NULL)
            FATAL("Could not create PNG");

        if ((info_ptr = png_create_info_struct(png_ptr)) == NULL)
            FATAL("Could not create PNG (2)");

        LOGD("Writing..");
#if SCREENSHOT_PNG_RGBA_WHEN_CONVENIENT
        if (screenBpp == 4) { // bitDepth * 4 / 8 = 4
            // Enable this to use 32-bit color RGBA color in order to memcpy the whole row
            png_set_IHDR(png_ptr, info_ptr, targetWidth, targetHeight, bitDepth, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
            row_pointers = (png_byte **) png_malloc(png_ptr, targetHeight * sizeof(png_byte *));
            for (int i = 0; i < targetHeight; i++) {
                row = (png_byte *) png_malloc(png_ptr, sizeof(png_byte) * targetWidth * screenBpp);
                row_pointers[i] = row;
                memcpy((char *) row, &((char *)vncbuf)[targetWidth * i * screenBpp], targetWidth * screenBpp);
                // row_pointers[i] = (png_byte *) &((char *)vncbuf)[targetWidth * i * screenBpp];
            }
        } else {
#endif
            // Use 24-bit color
            png_set_IHDR(png_ptr, info_ptr, targetWidth, targetHeight, bitDepth, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
            row_pointers = (png_byte **) png_malloc(png_ptr, targetHeight * sizeof(png_byte *));
            for (int i = 0; i < targetHeight; i++) {
                row = (png_byte *) png_malloc(png_ptr, sizeof(uint8_t) * targetWidth * bitDepth);
                row_pointers[i] = row;
                // TODO: optimize
                if (screenBpp == 1) {
                    // Assume RGB 332, convert to 8 bit depth
                    uint8_t pixel;
                    for (int j = 0; j < targetWidth; j++) {
                        pixel = ((uint8_t *)vncbuf)[i * targetWidth + j];
                        *row++ = (pixel & 3) << 6;
                        *row++ = ((pixel >> 2) & 7) << 5;
                        *row++ = pixel & (7 << 5);
                    }
                } else if (screenBpp == 2) {
                    // Assume RGB 565, convert to 8 bit depth
                    uint16_t pixel;
                    for (int j = 0; j < targetWidth; j++) {
                        pixel = ((uint16_t *)vncbuf)[i * targetWidth + j];
                        *row++ = (pixel & 31) << 3;
                        *row++ = ((pixel >> 5) & 63) << 2;
                        *row++ = ((pixel >> 11) & 31) << 3;
                    }
                } else if (screenBpp == 4) {
                    // Assume RGBA 8888
                    // TODO: BGRA 8888 support
                    uint32_t pixel;
                    for (int j = 0; j < targetWidth; j++) {
                        pixel = ((uint32_t *)vncbuf)[i * targetWidth + j];
                        *row++ = (pixel & 255);
                        *row++ = (pixel >> 8) & 255;
                        *row++ = (pixel >> 16) & 255;
                    }
                } else if (screenBpp == 8) {
                    uint64_t pixel;
                    for (int j = 0; j < targetWidth; j++) {
                        pixel = ((uint64_t *)vncbuf)[i * targetWidth + j];
                        *row++ = (pixel >> 8) & 255;
                        *row++ = (pixel >> 24) & 255;
                        *row++ = (pixel >> 40) & 255;
                    }
                }
            }
#if SCREENSHOT_PNG_RGBA_WHEN_CONVENIENT
        }
#endif
        // Write the png
        png_init_io(png_ptr, f);
        // png_set_filter(png_ptr, 0, PNG_FILTER_NONE);
        // png_set_filter(png_ptr, 0, PNG_FILTER_PAETH);
        // png_set_compression_level(png_ptr, Z_BEST_COMPRESSION);
        png_set_compression_level(png_ptr, 3);
        png_set_rows(png_ptr, info_ptr, row_pointers);
        png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, NULL);

        for (int i = 0; i < targetHeight; i++)
            png_free(png_ptr, row_pointers[i]);

        png_free(png_ptr, row_pointers);

        png_destroy_write_struct(&png_ptr, &info_ptr);

        if (ferror(f))
            FATAL("Could not write screenshot");

        if (fclose(f))
            FATAL("Could not close screenshot file");

        LOGD("Wrote PNG screenshot: %s", filename);
    } else {
        JpgEncoder encoder(4, 0);
        FILE *f;
        unsigned int len;

        if (!encoder.reserveData(targetWidth, targetHeight))
            FATAL("Could not reserve data for screenshot");

        LOGD("Encoding..");
        if (!encoder.encode(vncbuf, targetWidth, targetHeight, targetWidth, screenBpp, frame.format, SCREENSHOT_JPG_QUALITY))
            FATAL("Could not encode screenshot");

        len = encoder.getEncodedSize();
        if (!len)
            FATAL("Could not get encoded screenshot");

        fwrite(encoder.getEncodedData(), sizeof(char), len, f);

        if (ferror(f))
            FATAL("Could not write screenshot");

        if (fclose(f))
            FATAL("Could not close screenshot file");

        LOGD("Wrote JPEG screenshot: %s", filename);
    }
}

static void takeScreenshot(int signum, siginfo_t *siginfo, void *context) {
    LOGD("Received signal, taking screenshot..");
    writeScreenToFile((char *)SCREENSHOT_SIGNAL_FILE, vncscr->bitsPerPixel / 8);

    kill(siginfo->si_pid, SIGCONT);
    LOGD("Screenshot completed and response was sent.");
}

static void setupScreenshotSignalHandler() {
    struct sigaction act;
    memset(&act, 0x0, sizeof(struct sigaction));
    act.sa_sigaction = &takeScreenshot;
    act.sa_flags = SA_SIGINFO;

    if (sigaction(SIGCONT, &act, NULL) < 0)
        FATAL("Could not setup screenshot signal handler");
}

static int writeServerPid() {
    FILE *f;
    if ((f = fopen(PID_FILE, "w+")) == NULL) return 1;
    fprintf(f, "%d", getpid());
    if (fclose(f)) return 1;
    return 0;
}

static int deleteServerPid() {
    return unlink(PID_FILE);
}

static pid_t getRunningServerPid() {
    FILE *f = fopen(PID_FILE, "r");
    int i;
    char buf[16];
    memset(buf, 0x0, 16);
    fgets(buf, 15, f);
    int pid = atoi(buf);
    if (pid <= 0) return -1;
    if (kill(pid, 0) != 0) return -1;
    return (pid_t) pid;
}

static void waitForScreenshot(int signum) {
    LOGD("Received response, screenshot was successfully created.");
    cleanup(0);
}

static bool dispatchScreenshotSignal() {
    if (unlink(SCREENSHOT_SIGNAL_FILE) < 0)
        FATAL("Could not remove old screenshot " SCREENSHOT_SIGNAL_FILE);

    pid_t pid = getRunningServerPid();
    if (pid <= 0) return false;

    signal(SIGCONT, waitForScreenshot);
    kill(pid, SIGCONT);

    // Wait for 4 seconds
    sleep(4);

    // If we reach here, then the signal was not received before timeout

    // Remove the signal handler
    signal(SIGCONT, SIG_DFL);

    return false;
}

int main(int argc, char **argv)
{
    //pipe signals
    signal(SIGINT, quitSignal);
    signal(SIGKILL, quitSignal);
    signal(SIGILL, quitSignal);

    if(argc > 1) {
        int i = 1, r;
        while(i < argc) {
            if(*argv[i] == '-') {
                switch(*(argv[i] + 1)) {
                case 'h':
                    printUsage(argv);
                    return 0;
                case 'v':
                    P("androidvncserver version " VERSION);
                    return 0;
                case 'd':
                    if (++i >= argc) FATAL("No screen width provided");
                    screenWidth = atoi(argv[i]);
                    if (++i >= argc) FATAL("No screen height provided");
                    screenHeight = atoi(argv[i]);
                    break;
                case 'p':
                    if (++i >= argc) FATAL("No password provided");
                    strncpy(serverPassword,argv[i], 255);
                    break;
                case 'e':
                    if (++i >= argc) FATAL("No password file provided");
                    strncpy(serverPasswordFile, argv[i], 1023);
                    LOGD("Using %s", serverPasswordFile);
                    strcpy(serverPassword, "");
                    break;
                case 'b':
                    if (++i >= argc) FATAL("No bpp value provided");
                    desiredBpp = atoi(argv[i]);
                    if (desiredBpp != 1 &&
                        desiredBpp != 2 &&
                        desiredBpp != 4 &&
                        desiredBpp != 8)
                        FATAL("Unknown bpp value: %d", desiredBpp);
                    break;
                case 'z':
                    i++;
                    rotate180 = true;
                    break;
                case 'P':
                    if (++i >= argc) FATAL("No server port provided");
                    serverPort = atoi(argv[i]);
                    LOGD("Setting server port to %d", serverPort);
                    break;
                case 's':
                    if (++i >= argc) FATAL("No scaling value provided");
                    r = atoi(argv[i]);
                    if (r >= 1 && r <= 100) {
                        scaling = r;
                    } else {
                        FATAL("Invalid scaling value: %d%%", r);
                    }
                    LOGD("Scaling to %d%%",scaling);
                    break;
                case 'R':
                    if (++i >= argc) FATAL("No reverse host/port provided");
                    if (extractHostPort(&rhost, &rport, argv[i]))
                        FATAL("Could not read reverse host/port");
                    break;
                case 'r':
                    if (allowedRotation != ROT_ALL)
                        FATAL("Cannot specify -r and -o together");

                    if (++i >= argc) FATAL("No rotation provided");
                    r = atoi(argv[i]);
                    switch (r) {
                    case 90:
                        allowedRotation = ROT_90;
                        break;
                    case 180:
                        allowedRotation = ROT_180;
                        break;
                    case 270:
                        allowedRotation = ROT_270;
                        break;
                    case 360:
                        r = 0;
                    case 0:
                        allowedRotation = ROT_0;
                        break;
                    default: FATAL("Unknown rotation value: %d", r);
                    }
                    LOGD("Forcing %d degree screen rotation", r);
                    break;
                case 'o':
                    if (allowedRotation != ROT_ALL)
                        FATAL("Cannot specify -o and -r together");

                    if (++i >= argc) FATAL("No orientation provided");
                    for (unsigned int j = 0; j < strlen(argv[i]); j++) {
                        char c = argv[i][j];
                        if (c < 'A' || c > 'Z') continue;
                        argv[i][j] = c + ('a' - 'A');
                    }
                    if (!strcmp(argv[i], "portrait")) {
                        allowedRotation = ROT_0 | ROT_180;
                        LOGD("Forcing portrait orientation");
                    } else if (!strcmp(argv[i], "landscape")) {
                        allowedRotation = ROT_90 | ROT_270;
                        LOGD("Forcing landscape orientation");
                    } else {
                        FATAL("Unknown orientation: %s", argv[i]);
                    }
                    break;
                case 'f':
                    skipFrames = true;
                    LOGD("Enabled frame skipping");
                    break;
                case 'S':
                    if (++i >= argc) FATAL("No screenshot filename provided");
                    screenshotFile = argv[i];
                    LOGD("Set screenshot file: %s", screenshotFile);
                    break;
                case 'U':
                    screenshotFile = (char *) SCREENSHOT_SIGNAL_FILE;
                    screenshotFast = true;
                    LOGD("Attempting fast screenshot: %s", screenshotFile);
                    break;
                case 'X':
                    screenshotSkipFrame = true;
                    LOGD("Skipping first frame for screenshot");
                    break;
                }
            }
            i++;
        }
    }

    // Try to shortcut out by finding a running droidvncserver
    if (screenshotFast) {
        if (dispatchScreenshotSignal()) {
            return 0;
        } else {
            LOGD("Fast screenshot failed, continuing normal screenshot");
        }
    }

    LOGD("Starting rotation watcher");

    if (start_rotation_watcher())
        FATAL("Could not start rotation watcher");

    LOGD("Starting thread pool");

    minicap_start_thread_pool();

    // Get the screen dimensions if not provided
    if (screenWidth <= 0 || screenHeight <= 0) {
        LOGD("Querying display info (1)");
        if (try_get_dumpsys_display_info(&screenWidth, &screenHeight) != 0) {
            LOGD("Querying display info (2)");
            if (try_get_framebuffer_display_info(&screenWidth, &screenHeight) != 0) {
                LOGD("Querying display info (3)");
                Minicap::DisplayInfo fbInfo;
                if (!minicap_try_get_display_info(0, &fbInfo)) {
                    screenWidth = fbInfo.width;
                    screenHeight = fbInfo.height;
                } else  {
                    FATAL("Unable to get display info");
                }
            }
        }
    }

    LOGD("Screen dimensions: %dx%d", screenWidth, screenHeight);

    forcedRotation = allowedRotation != ROT_ALL;

    if ((minicap = minicap_create(0)) == NULL) {
        FATAL("Could not create minicap object");
    }

    switch (minicap->getCaptureMethod()) {
    case Minicap::METHOD_FRAMEBUFFER:
        LOGD("Display method: framebuffer");
        break;
    case Minicap::METHOD_SCREENSHOT:
        LOGD("Display method: screenshot");
        break;
    case Minicap::METHOD_VIRTUAL_DISPLAY:
        LOGD("Display method: virtual display");
        break;
    }

    Minicap::DisplayInfo realInfo;
    realInfo.width = screenWidth;
    realInfo.height = screenHeight;
    if (minicap->setRealInfo(realInfo) != 0)
        FATAL("Could not set real display info");

    // Get and set original orientation
    while (!check_rotation_change(&screenRotation));
    LOGD("Original rotation: %d", screenRotation);
    imageRotation = getImageRotation();

    FrameWaiter *gWaiter = new FrameWaiter;
    minicap->setFrameAvailableListener(gWaiter);

    resetMinicapOrientation();

    // Grab the first frame so we can check its properties
    if (gWaiter->isStopped())
        FATAL("Frame waiter not started");

    gWaiter->waitForFrame();
    if (minicap->consumePendingFrame(&frame) != 0)
        FATAL("Could not read first frame");

    LOGD("Bytes per pixel: %d", frame.bpp);
    LOGD("Image format: %s", getImageFormatName());

    unsigned int targetBpp = frame.bpp;
    void (*updateScreenFn)(int);
    void (*setupScreenFn)();

    if (screenshotFile == NULL && (desiredBpp > 0 && (unsigned int) desiredBpp != frame.bpp)) {
        // Only support 4 -> 2 for now
        switch (frame.bpp) {
        case 1:
            setupScreenFn = &setupScreen1;
            updateScreenFn = &updateScreen1;
            break;
        case 2:
            setupScreenFn = &setupScreen2;
            updateScreenFn = &updateScreen2;
            break;
        case 4:
            setupScreenFn = &setupScreen4;
            updateScreenFn = &updateScreen4;
            if (desiredBpp <= 2) {
                // Convert to RGB_565
                setupScreenFn = &setupScreen42;
                updateScreenFn = &updateScreen42;
                targetBpp = 2;
            }
            break;
        case 8:
            setupScreenFn = &setupScreen8;
            updateScreenFn = &updateScreen8;
            break;
        default:
            FATAL("Unsupported bpp: %d", frame.bpp);
        }
    } else {
        switch (frame.bpp) {
        case 1:
            setupScreenFn = &setupScreen1;
            updateScreenFn = &updateScreen1;
            break;
        case 2:
            setupScreenFn = &setupScreen2;
            updateScreenFn = &updateScreen2;
            break;
        case 4:
            setupScreenFn = &setupScreen4;
            updateScreenFn = &updateScreen4;
            break;
        case 8:
            setupScreenFn = &setupScreen8;
            updateScreenFn = &updateScreen8;
            break;
        default:
            FATAL("Unsupported bpp: %d", frame.bpp);
        }
    }

    if (screenshotFile != NULL) {
        if (screenshotSkipFrame) {
            // On some devices, the first frame seems to be a black screen, so skip to the next one when enabled
            minicap->releaseConsumedFrame(&frame);
            gWaiter->waitForFrame();
            minicap->consumePendingFrame(&frame);
        }

        // Stop getting informed of more frames on other thread(s)
        minicap->setFrameAvailableListener(NULL);

        if ((vncbuf = (unsigned char *)malloc(frame.width * frame.height * frame.bpp)) == NULL)
            FATAL("Could not create buffer");

        // Write image to vncbuf
        updateScreenFn(imageRotation);

        writeScreenToFile(screenshotFile, targetBpp);

        free(vncbuf);

        minicap->releaseConsumedFrame(&frame);
        minicap_free(minicap);
        return 0;
    }

    findKeyAndTouchDevice();
    LOGD("Initializing touch device %s ...", TOUCH_DEVICE);
    init_touch();
    LOGD("Initializing keyboard device %s ...", KBD_DEVICE);
    if (strcmp(TOUCH_DEVICE, KBD_DEVICE)!=0) {
        init_kbd();
    }

    initVncServer(argc, argv, frame.width, frame.height, frame.stride, targetBpp);
    (*setupScreenFn)();

    unsigned int expectedFrameSize = frame.stride * frame.height * frame.bpp;
    if (expectedFrameSize != frame.size)
        FATAL("Unexpected frame size %d, expected %d", frame.size, expectedFrameSize);

    // Send the first frame
    (*updateScreenFn)(imageRotation);
    rfbMarkRectAsModified(vncscr, 0, 0, vncscr->width, vncscr->height);

    minicap->releaseConsumedFrame(&frame);

    LOGD("VNC server initialized");

    if (rhost) {
        rfbClientPtr cl;
        cl = rfbReverseConnection(vncscr, rhost, rport);
        if (cl == NULL) {
            LOGD("Couldn't connect to remote host");
        } else {
            LOGD("Starting on hold client");
            cl->onHold = FALSE;
            rfbStartOnHoldClient(cl);
        }
    }

    // long usec;
    rfbRunEventLoop(vncscr, -1, TRUE);

    setupScreenshotSignalHandler();
    writeServerPid();

    int x, y, pending, err;

    while (1) {
        if (check_rotation_change(&screenRotation)) {
            LOGD("Screen rotation changed: %d", screenRotation);

            imageRotation = getImageRotation();

            // Stop old minicap instance
            minicap->setFrameAvailableListener(NULL);
            gWaiter->stop();
            minicap_free(minicap);
            delete gWaiter;

            // Wait for 0.1ms
            usleep(100000);

            // Create new minicap instance
            minicap = minicap_create(0);
            gWaiter = new FrameWaiter;
            minicap->setFrameAvailableListener(gWaiter);
            resetMinicapOrientation();
            gWaiter->waitForFrame();
            minicap->consumePendingFrame(&frame);

            LOGD("Got first re-oriented frame: width=%d height=%d stride=%d bpp=%d", frame.width, frame.height, frame.stride, frame.bpp);

            expectedFrameSize = frame.stride * frame.height * frame.bpp;
            if (frame.size != expectedFrameSize)
                FATAL("Unexpected frame size %d, expected %d", frame.size, expectedFrameSize);

            // Reinitialize VNC screen
            reinitVncServer(frame.width, frame.height, frame.stride, targetBpp);

            // Send the first frame
            (*updateScreenFn)(imageRotation);
            rfbMarkRectAsModified(vncscr, 0, 0, vncscr->width, vncscr->height);

            minicap->releaseConsumedFrame(&frame);
        }

        if (!gWaiter->isStopped() && (pending = gWaiter->waitForFrame()) > 0) {
            if (skipFrames && pending > 1) {
                // Skip frames if we have too many. Not particularly thread safe,
                // but this loop should be the only consumer anyway (i.e. nothing
                // else decreases the frame count).
                gWaiter->reportExtraConsumption(pending - 1);

                while (--pending >= 1) {
                    if ((err = minicap->consumePendingFrame(&frame)) != 0) {
                        if (err == -EINTR) {
                            LOGE("Frame consumption interrupted by EINTR");
                        }
                        else {
                            LOGE("Unable to skip pending frame");
                        }
                    }
                    minicap->releaseConsumedFrame(&frame);
                }
                // Process the one remaining frame
                if ((err = minicap->consumePendingFrame(&frame)) == 0) {
                    (*updateScreenFn)(imageRotation);
                    rfbMarkRectAsModified(vncscr, 0, 0, vncscr->width, vncscr->height);
                } else {
                    if (err == -EINTR) {
                        LOGD("Frame consumption interrupted by EINTR");
                    }
                    else {
                        LOGE("Unable to consume pending frame");
                    }
                }
                minicap->releaseConsumedFrame(&frame);
            } else {
                // Consume all available frames
                do {
                    if ((err = minicap->consumePendingFrame(&frame)) == 0) {
                        (*updateScreenFn)(imageRotation);
                        rfbMarkRectAsModified(vncscr, 0, 0, vncscr->width, vncscr->height);
                    } else {
                        if (err == -EINTR) {
                            LOGD("Frame consumption interrupted by EINTR");
                        }
                        else {
                            LOGE("Unable to consume pending frame");
                        }
                    }
                    minicap->releaseConsumedFrame(&frame);
                } while ((pending = gWaiter->getPendingFrames()) > 0);
            }
        }
    }

    LOGD("Finishing..");
    cleanup(0);
}

