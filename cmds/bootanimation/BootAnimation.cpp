/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "BootAnimation"

#include <stdint.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <math.h>
#include <fcntl.h>
#include <utils/misc.h>
#include <signal.h>
#include <dirent.h>

#include <linux/input.h>

#include <binder/IPCThreadState.h>
#include <utils/threads.h>
#include <utils/Atomic.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/AssetManager.h>

#include <ui/PixelFormat.h>
#include <ui/Rect.h>
#include <ui/Region.h>
#include <ui/DisplayInfo.h>
#include <ui/FramebufferNativeWindow.h>
#include <ui/EGLUtils.h>

#include <surfaceflinger/ISurfaceComposer.h>
#include <surfaceflinger/ISurfaceComposerClient.h>

#include <core/SkBitmap.h>
#include <images/SkImageDecoder.h>

#include <GLES/gl.h>
#include <GLES/glext.h>
#include <EGL/eglext.h>

#include <cutils/logd.h>
#include <cutils/logprint.h>
#include <cutils/event_tag_map.h>
#include <cutils/logger.h>

#include "BootAnimation.h"

namespace android {

// ---------------------------------------------------------------------------

BootAnimation::BootAnimation() : Thread(false)
{
    mSession = new SurfaceComposerClient();
}

BootAnimation::~BootAnimation() {
}

void BootAnimation::onFirstRef() {
    status_t err = mSession->linkToComposerDeath(this);
    LOGE_IF(err, "linkToComposerDeath failed (%s) ", strerror(-err));
    if (err == NO_ERROR) {
        run("BootAnimation", PRIORITY_DISPLAY);
    }
}

sp<SurfaceComposerClient> BootAnimation::session() const {
    return mSession;
}


void BootAnimation::binderDied(const wp<IBinder>& who)
{
    // woah, surfaceflinger died!
    LOGD("SurfaceFlinger died, exiting...");

    // calling requestExit() is not enough here because the Surface code
    // might be blocked on a condition variable that will never be updated.
    kill( getpid(), SIGKILL );
    requestExit();
}

status_t BootAnimation::initTexture(Texture* texture, AssetManager& assets,
        const char* name) {
    Asset* asset = assets.open(name, Asset::ACCESS_BUFFER);
    if (!asset)
        return NO_INIT;
    SkBitmap bitmap;
    SkImageDecoder::DecodeMemory(asset->getBuffer(false), asset->getLength(),
            &bitmap, SkBitmap::kNo_Config, SkImageDecoder::kDecodePixels_Mode);
    asset->close();
    delete asset;

    // ensure we can call getPixels(). No need to call unlock, since the
    // bitmap will go out of scope when we return from this method.
    bitmap.lockPixels();

    const int w = bitmap.width();
    const int h = bitmap.height();
    const void* p = bitmap.getPixels();

    GLint crop[4] = { 0, h, w, -h };
    texture->w = w;
    texture->h = h;

    glGenTextures(1, &texture->name);
    glBindTexture(GL_TEXTURE_2D, texture->name);

    switch (bitmap.getConfig()) {
        case SkBitmap::kA8_Config:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA,
                    GL_UNSIGNED_BYTE, p);
            break;
        case SkBitmap::kARGB_4444_Config:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                    GL_UNSIGNED_SHORT_4_4_4_4, p);
            break;
        case SkBitmap::kARGB_8888_Config:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, p);
            break;
        case SkBitmap::kRGB_565_Config:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB,
                    GL_UNSIGNED_SHORT_5_6_5, p);
            break;
        default:
            break;
    }

    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, crop);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return NO_ERROR;
}

status_t BootAnimation::initTexture(void* buffer, size_t len)
{
    //StopWatch watch("blah");

    SkBitmap bitmap;
    SkImageDecoder::DecodeMemory(buffer, len,
            &bitmap, SkBitmap::kRGB_565_Config,
            SkImageDecoder::kDecodePixels_Mode);

    // ensure we can call getPixels(). No need to call unlock, since the
    // bitmap will go out of scope when we return from this method.
    bitmap.lockPixels();

    const int w = bitmap.width();
    const int h = bitmap.height();
    const void* p = bitmap.getPixels();

    GLint crop[4] = { 0, h, w, -h };
    int tw = 1 << (31 - __builtin_clz(w));
    int th = 1 << (31 - __builtin_clz(h));
    if (tw < w) tw <<= 1;
    if (th < h) th <<= 1;

    switch (bitmap.getConfig()) {
        case SkBitmap::kARGB_8888_Config:
            if (tw != w || th != h) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA,
                        GL_UNSIGNED_BYTE, 0);
                glTexSubImage2D(GL_TEXTURE_2D, 0,
                        0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, p);
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA,
                        GL_UNSIGNED_BYTE, p);
            }
            break;

        case SkBitmap::kRGB_565_Config:
            if (tw != w || th != h) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tw, th, 0, GL_RGB,
                        GL_UNSIGNED_SHORT_5_6_5, 0);
                glTexSubImage2D(GL_TEXTURE_2D, 0,
                        0, 0, w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, p);
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tw, th, 0, GL_RGB,
                        GL_UNSIGNED_SHORT_5_6_5, p);
            }
            break;
        default:
            break;
    }

    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, crop);

    return NO_ERROR;
}

status_t BootAnimation::readyToRun() {
    mAssets.addDefaultAssets();

    DisplayInfo dinfo;
    status_t status = session()->getDisplayInfo(0, &dinfo);
    if (status)
        return -1;

    // create the native surface
    sp<SurfaceControl> control = session()->createSurface(
            getpid(), 0, dinfo.w, dinfo.h, PIXEL_FORMAT_RGB_565);
    session()->openTransaction();
    control->setLayer(0x40000000);
    session()->closeTransaction();

    sp<Surface> s = control->getSurface();

    // initialize opengl and egl
    const EGLint attribs[] = {
            EGL_DEPTH_SIZE, 0,
            EGL_NONE
    };
    EGLint w, h, dummy;
    EGLint numConfigs;
    EGLConfig config;
    EGLSurface surface;
    EGLContext context;

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

    eglInitialize(display, 0, 0);
    EGLUtils::selectConfigForNativeWindow(display, attribs, s.get(), &config);
    surface = eglCreateWindowSurface(display, config, s.get(), NULL);
    context = eglCreateContext(display, config, NULL, NULL);
    eglQuerySurface(display, surface, EGL_WIDTH, &w);
    eglQuerySurface(display, surface, EGL_HEIGHT, &h);

    if (eglMakeCurrent(display, surface, surface, context) == EGL_FALSE)
        return NO_INIT;

    mDisplay = display;
    mContext = context;
    mSurface = surface;
    mWidth = w;
    mHeight = h;
    mFlingerSurfaceControl = control;
    mFlingerSurface = s;

    mAndroidAnimation = false;
    status_t err = mZip.open("/data/local/bootanimation.zip");
    if (err != NO_ERROR) {
        err = mZip.open("/system/media/bootanimation.zip");
        if (err != NO_ERROR) {
            mAndroidAnimation = true;
        }
    }

    mDisplayPriority = ANDROID_LOG_SILENT;
    initInput();
    initFont();
    initBuffer();

    return NO_ERROR;
}

bool BootAnimation::threadLoop()
{
    bool r = true;
    while (r) {
        mSwitching = false;
        if (mDisplayPriority < ANDROID_LOG_SILENT) {
            r = text();
        } else if (mAndroidAnimation) {
            r = android();
        } else {
            r = movie();
        }
    }

    glDeleteTextures(1, &mFontTex.name);
    eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(mDisplay, mContext);
    eglDestroySurface(mDisplay, mSurface);
    mFlingerSurface.clear();
    mFlingerSurfaceControl.clear();
    eglTerminate(mDisplay);
    IPCThreadState::self()->stopProcess();
    return r;
}

bool BootAnimation::initFont()
{
    initTexture(&mFontTex, mAssets, "images/font_10x18.png");
    mFontWidth = 10;
    mFontHeight = 18;
    mCols = mWidth / mFontWidth;
    mRows = mHeight / mFontHeight;
    return true;
}

bool BootAnimation::initBuffer() {
    if (mCols > 0 && mRows > 0) {
        mLineBuffer = new char*[mRows];
        if (mLineBuffer) {
            for (int i = 0; i<mRows; i++) {
                mLineBuffer[i] = new char[mCols + 1];
                for (int j = 0; j<mCols + 1; j++) {
                    mLineBuffer[i][j] = '\0';
                }
            }
        }
    } else {
        mLineBuffer = NULL;
    }
    mBufferPos = 0;

    return true;
}

void BootAnimation::printLine(char *s) {
    int col = 0;
    char c;
    while ((c = *s++))
    {
        mLineBuffer[mBufferPos][col++] = c;
        if (col >= mCols) {
            col = 0;
            mBufferPos = (mBufferPos + 1) % mRows;
        }
    }
    while (col<mCols) {
        mLineBuffer[mBufferPos][col++] = '\0';
    }
    mBufferPos = (mBufferPos + 1) % mRows;
}

void BootAnimation::replaceLine(char *s) {
    for (int i=0; i<mCols; i++) {
        mLineBuffer[(mRows + mBufferPos - 1) % mRows][i] = s[i];
        if (s[i] == '\0') {
            break;
        }
    }
}
void BootAnimation::drawText() {
    glBindTexture(GL_TEXTURE_2D, mFontTex.name);
    int xpos = 0;
    int ypos = mHeight;
    char *s;
    char c;
    for (int i = 0; i<mRows; i++) {
        s = mLineBuffer[(mBufferPos + i) % mRows];
        while ((c = *s++)) {
            c -= 32;
            if (c < 96) {
                int textCrop[4] = {(c % 24) * mFontWidth, (c / 24 + 1) * mFontHeight, mFontWidth, -mFontHeight};
                glTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_CROP_RECT_OES,textCrop);
                glDrawTexiOES(xpos, ypos, 0, mFontWidth, mFontHeight);
            }
            xpos += mFontWidth;
        }
        ypos -= mFontHeight;
        xpos = 0;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return;
}

bool BootAnimation::initLogDevice() {
    nStartups = 0;
    const char* devname[N_LOG_DEVICES] = {"/dev/"LOGGER_LOG_MAIN,"/dev/"LOGGER_LOG_SYSTEM};
    for (int i = 0; i < N_LOG_DEVICES; i++) {
        mLogDevices[i] = open(devname[i],O_RDONLY|O_NONBLOCK);
        if (mLogDevices[i] < 0) {
            LOGE("unable to open log device %s, %s",devname[i],strerror(errno));
            return false;
        }
    }
    return true;
}

bool BootAnimation::initInput() {
    DIR *inputdir = opendir("/dev/input/");
    if (inputdir == 0) {
        mInputDevice = 0;
        LOGE("unable to open input device directory");
        return false;
    }
    struct dirent *de;
    int fd;
    while ((de = readdir(inputdir))) { 
        if (strncmp(de->d_name,"event",5)) {
            continue;
        }
        mInputDevice = openat(dirfd(inputdir),de->d_name,O_RDONLY|O_NONBLOCK);
        if (mInputDevice < 0) {
            LOGE("unable to open input device %s, %s",de->d_name,strerror(errno));
            continue;
        }
        uint8_t key_bitmask[KEY_MAX / 8 + 1];
        memset (key_bitmask,0,sizeof(key_bitmask));

        int ret = ioctl(mInputDevice,EVIOCGBIT(EV_KEY,sizeof(key_bitmask)),key_bitmask);
        if (ret < 0) {
            LOGE("error getting keys for device %s, %s",de->d_name,strerror(errno));
            close(mInputDevice);
            continue;
        }
#define KEY_IN_BITMASK(x,y) ((x)[(y) / 8] & ((1 << (y) % 8)))
        if (KEY_IN_BITMASK(key_bitmask,KEY_VOLUMEUP) ||
            KEY_IN_BITMASK(key_bitmask,KEY_VOLUMEDOWN)) {
            LOGD("found device with required keys: %s", de->d_name);
            return true;
        }
#undef KEY_IN_BITMASK
        close(mInputDevice);
        mInputDevice = 0;
    }
    LOGE("unable to find input device with required keys");
    return false;
}

bool BootAnimation::checkInput() {
    if (mInputDevice < 0) {
        return false;
    }
    struct input_event buf[1];
    if (read(mInputDevice,buf,sizeof(struct input_event) * 1) > 0) {
        //LOGD("button pressed, %x %x %x",buf[0].type,buf[0].code,buf[0].value);
        if (buf[0].type | EV_KEY && buf[0].value == 1) {
            if (buf[0].code == KEY_VOLUMEUP) {
                mSwitching = true;
                mDisplayPriority -= 1;
                return true;
            }
            if (buf[0].code == KEY_VOLUMEDOWN) {
                mSwitching = true;
                mDisplayPriority += 1;
                return true;
            }
        }
    }
    return false;
}

bool BootAnimation::processLog()
{
    union logger_buf{
        unsigned char buf[LOGGER_ENTRY_MAX_LEN + 1] __attribute__((aligned(4)));
        struct logger_entry entry __attribute__((aligned(4)));
    };
    union logger_buf rdbuf;
    union logger_buf entries[N_LOG_DEVICES];
    struct logger_entry* nextEntry = NULL;
    bool linesAdded = false;
    while (true) {
        int ret;
        if (nextEntry != NULL)
            nextEntry->len = 0;
        for (int i = 0; i < N_LOG_DEVICES; i++) {
            if (mLogDevices[i] <= 0) {
                return false;
                LOGE("log device not open");
            }
            if (entries[i].entry.len == 0) {
                ret = read(mLogDevices[i],entries[i].buf,LOGGER_ENTRY_MAX_LEN);
            } else {
                continue;
            }
            if (ret < 0) {
                if (errno == EAGAIN) {
                    entries[i].entry.len = 0;
                    continue;
                } else {
                    LOGE("unable to read from log device %d, %s",mLogDevices[i],strerror(errno));
                    return false;
                }
            }
            if (ret == 0) {
                LOGE("unexpected EOF on device %d!",mLogDevices[i]);
                return false;
            }
        }
        nextEntry = NULL;
        for (int i = 0; i < N_LOG_DEVICES; i++) {
            if (entries[i].entry.len == 0) {
                continue;
            }
            if (nextEntry == NULL) {
                nextEntry = &entries[i].entry;
                continue;
            }
            if (nextEntry != NULL) {
                if (nextEntry->sec > entries[i].entry.sec || 
                    (nextEntry->sec == entries[i].entry.sec &&
                     nextEntry->nsec > entries[i].entry.nsec)) {
                    nextEntry = &entries[i].entry;
                }
            }
        }

        if (nextEntry == NULL) {
            break;
        }
        AndroidLogEntry processedEntry;
        if ( 0 != android_log_processLogBuffer(nextEntry,&processedEntry) ) {
            LOGE("error processing log buffer");
            return false;
        }
        int linelen = processedEntry.messageLen + strlen(processedEntry.tag) + 5;
        char retstr[LOGGER_ENTRY_MAX_LEN];
        if (mDisplayPriority != ANDROID_LOG_FATAL) {
            if (processedEntry.priority < mDisplayPriority) {
                continue;
            }
            snprintf(retstr,linelen + 1, "%s: %s",processedEntry.tag,processedEntry.message);
            printLine(retstr);
            linesAdded = true;
        } else {
            if (!strncmp(processedEntry.tag,"SystemServer",12)) {
                snprintf(retstr,linelen + 1, "%s: %s",processedEntry.tag,processedEntry.message);
                printLine(retstr);
                linesAdded = true;
            }
            if (!strncmp(processedEntry.tag,"installd",5)) {
                if (!strncmp(processedEntry.message,"DexInv: --- BEGIN",15)) {
                    snprintf(retstr,linelen + 1, "%s: %s",processedEntry.tag,processedEntry.message);
                    LOGD("match: %s",retstr);
                    printLine(retstr);
                    linesAdded = true;
                }
            }
            if (!strncmp(processedEntry.tag,"PackageManager",14)) {
                if (!strncmp(processedEntry.message,"Unpacking native libraries",14)) {
                    snprintf(retstr,linelen + 1, "%s",processedEntry.message);
                    replaceLine(retstr);
                    linesAdded = true;
                }
            }
            if (!strncmp(processedEntry.tag,"AndroidRuntime",14)) {
                if (!strncmp(processedEntry.message,">>>>>>>>>>>>>> AndroidRuntime START <<<<<<<<<<<<<<",30)) {
                    nStartups++;
                }
                snprintf(retstr,linelen + 1, "%s: %s",processedEntry.tag,processedEntry.message);
                printLine(retstr);
                linesAdded = true;
            }
        }
        //snprintf(retstr,linelen + 1, "%s: %s",processedEntry.tag,processedEntry.message);
        //printLine(retstr);
        //linesAdded = true;
        //break;
    }
    return linesAdded;
}

bool BootAnimation::android()
{
    initTexture(&mAndroid[0], mAssets, "images/android-logo-mask.png");
    initTexture(&mAndroid[1], mAssets, "images/android-logo-shine.png");

    // clear screen
    glShadeModel(GL_FLAT);
    glDisable(GL_DITHER);
    glDisable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(mDisplay, mSurface);

    glEnable(GL_TEXTURE_2D);
    glTexEnvx(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    const GLint xc = (mWidth  - mAndroid[0].w) / 2;
    const GLint yc = (mHeight - mAndroid[0].h) / 2;
    const Rect updateRect(xc, yc, xc + mAndroid[0].w, yc + mAndroid[0].h);

    // draw and update only what we need
    mFlingerSurface->setSwapRectangle(updateRect);

    glScissor(updateRect.left, mHeight - updateRect.bottom, updateRect.width(),
            updateRect.height());

    // Blend state
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glTexEnvx(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    const nsecs_t startTime = systemTime();
    do {
        checkInput();
        nsecs_t now = systemTime();
        double time = now - startTime;
        float t = 4.0f * float(time / us2ns(16667)) / mAndroid[1].w;
        GLint offset = (1 - (t - floorf(t))) * mAndroid[1].w;
        GLint x = xc - offset;

        glDisable(GL_SCISSOR_TEST);
        glClear(GL_COLOR_BUFFER_BIT);

        glEnable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glBindTexture(GL_TEXTURE_2D, mAndroid[1].name);
        glDrawTexiOES(x,                 yc, 0, mAndroid[1].w, mAndroid[1].h);
        glDrawTexiOES(x + mAndroid[1].w, yc, 0, mAndroid[1].w, mAndroid[1].h);

        glEnable(GL_BLEND);
        glBindTexture(GL_TEXTURE_2D, mAndroid[0].name);
        glDrawTexiOES(xc, yc, 0, mAndroid[0].w, mAndroid[0].h);

        EGLBoolean res = eglSwapBuffers(mDisplay, mSurface);
        if (res == EGL_FALSE)
            break;

        // 12fps: don't animate too fast to preserve CPU
        const nsecs_t sleepTime = 83333 - ns2us(systemTime() - now);
        if (sleepTime > 0)
            usleep(sleepTime);
    } while (!exitPending() && !mSwitching);

    glDeleteTextures(1, &mAndroid[0].name);
    glDeleteTextures(1, &mAndroid[1].name);
    if (mSwitching && !exitPending()) {
        return true;
    } else {
        return false;
    }
}

bool BootAnimation::text() {
    initTexture(&mAndroid[0], mAssets, "images/android-text-header.png");

    glShadeModel(GL_FLAT);
    glDisable(GL_DITHER);
    glDisable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(mDisplay, mSurface);

    glEnable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glTexEnvx(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    bool shouldDraw = true;
    for (int i=0; i<mRows; i++) {
        printLine("-");
    }
    initLogDevice();
    do {
        nsecs_t now = systemTime();
        checkInput();
        if (nStartups > 1) {
            printLine("!!!!!!!!!!!!!!!!!!!!!!!!");
            printLine("!! Boot loop detected !!");
            printLine("!!!!!!!!!!!!!!!!!!!!!!!!");
        }

        if (shouldDraw) {
            glDisable(GL_BLEND);
            glClear(GL_COLOR_BUFFER_BIT);
            drawText();
            glEnable(GL_BLEND);
            glBindTexture(GL_TEXTURE_2D, mAndroid[0].name);
            glDrawTexiOES(0, mHeight - mAndroid[0].h, 0, mAndroid[0].w, mAndroid[0].h);

            EGLBoolean res = eglSwapBuffers(mDisplay, mSurface);
            if (res == EGL_FALSE)
                break;
        }
        shouldDraw = processLog();

        //5 fps max, drawing text is CPU expensive
        const nsecs_t sleepTime = 200000 - ns2us(systemTime() - now);
        if (sleepTime > 0)
            usleep(sleepTime);
    } while (!exitPending() && !mSwitching);
    for (int i=0; i < N_LOG_DEVICES; i++) {
        close(mLogDevices[i]);
        mLogDevices[i] = 0;
    }

    glDeleteTextures(1, &mAndroid[0].name);
    if (mSwitching && !exitPending()) {
        return true;
    } else {
        return false;
    }
}

bool BootAnimation::movie()
{
    ZipFileRO& zip(mZip);

    size_t numEntries = zip.getNumEntries();
    ZipEntryRO desc = zip.findEntryByName("desc.txt");
    FileMap* descMap = zip.createEntryFileMap(desc);
    LOGE_IF(!descMap, "descMap is null");
    if (!descMap) {
        return false;
    }

    String8 desString((char const*)descMap->getDataPtr(),
            descMap->getDataLength());
    char const* s = desString.string();

    Animation animation;

    // Parse the description file
    for (;;) {
        const char* endl = strstr(s, "\n");
        if (!endl) break;
        String8 line(s, endl - s);
        const char* l = line.string();
        int fps, width, height, count, pause;
        char path[256];
        if (sscanf(l, "%d %d %d", &width, &height, &fps) == 3) {
            //LOGD("> w=%d, h=%d, fps=%d", fps, width, height);
            animation.width = width;
            animation.height = height;
            animation.fps = fps;
        }
        if (sscanf(l, "p %d %d %s", &count, &pause, path) == 3) {
            //LOGD("> count=%d, pause=%d, path=%s", count, pause, path);
            Animation::Part part;
            part.count = count;
            part.pause = pause;
            part.path = path;
            animation.parts.add(part);
        }
        s = ++endl;
    }

    // read all the data structures
    const size_t pcount = animation.parts.size();
    for (size_t i=0 ; i<numEntries ; i++) {
        char name[256];
        ZipEntryRO entry = zip.findEntryByIndex(i);
        if (zip.getEntryFileName(entry, name, 256) == 0) {
            const String8 entryName(name);
            const String8 path(entryName.getPathDir());
            const String8 leaf(entryName.getPathLeaf());
            if (leaf.size() > 0) {
                for (int j=0 ; j<pcount ; j++) {
                    if (path == animation.parts[j].path) {
                        int method;
                        // supports only stored png files
                        if (zip.getEntryInfo(entry, &method, 0, 0, 0, 0, 0)) {
                            if (method == ZipFileRO::kCompressStored) {
                                FileMap* map = zip.createEntryFileMap(entry);
                                if (map) {
                                    Animation::Frame frame;
                                    frame.name = leaf;
                                    frame.map = map;
                                    Animation::Part& part(animation.parts.editItemAt(j));
                                    part.frames.add(frame);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // clear screen
    glShadeModel(GL_FLAT);
    glDisable(GL_DITHER);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT);

    eglSwapBuffers(mDisplay, mSurface);

    glBindTexture(GL_TEXTURE_2D, 0);
    glEnable(GL_TEXTURE_2D);
    glTexEnvx(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    const int xc = (mWidth - animation.width) / 2;
    const int yc = ((mHeight - animation.height) / 2);
    nsecs_t lastFrame = systemTime();
    nsecs_t frameDuration = s2ns(1) / animation.fps;

    Region clearReg(Rect(mWidth, mHeight));
    clearReg.subtractSelf(Rect(xc, yc, xc+animation.width, yc+animation.height));

    for (int i=0 ; i<pcount && !exitPending() && !mSwitching; i++) {
        const Animation::Part& part(animation.parts[i]);
        const size_t fcount = part.frames.size();
        glBindTexture(GL_TEXTURE_2D, 0);

        for (int r=0 ; (!part.count || r<part.count) && !exitPending() && !mSwitching ; r++) {
            for (int j=0 ; j<fcount && !exitPending() && !mSwitching; j++) {
                checkInput();
                const Animation::Frame& frame(part.frames[j]);

                if (r > 0) {
                    glBindTexture(GL_TEXTURE_2D, frame.tid);
                } else {
                    if (part.count != 1) {
                        glGenTextures(1, &frame.tid);
                        glBindTexture(GL_TEXTURE_2D, frame.tid);
                        glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    }
                    initTexture(
                            frame.map->getDataPtr(),
                            frame.map->getDataLength());
                }

                if (!clearReg.isEmpty()) {
                    Region::const_iterator head(clearReg.begin());
                    Region::const_iterator tail(clearReg.end());
                    glEnable(GL_SCISSOR_TEST);
                    while (head != tail) {
                        const Rect& r(*head++);
                        glScissor(r.left, mHeight - r.bottom,
                                r.width(), r.height());
                        glClear(GL_COLOR_BUFFER_BIT);
                    }
                    glDisable(GL_SCISSOR_TEST);
                }
                glDrawTexiOES(xc, yc, 0, animation.width, animation.height);
                eglSwapBuffers(mDisplay, mSurface);

                nsecs_t now = systemTime();
                nsecs_t delay = frameDuration - (now - lastFrame);
                lastFrame = now;
                long wait = ns2us(frameDuration);
                if (wait > 0)
                    usleep(wait);
            }
            usleep(part.pause * ns2us(frameDuration));
        }

        // free the textures for this part
        if (part.count != 1) {
            for (int j=0 ; j<fcount ; j++) {
                const Animation::Frame& frame(part.frames[j]);
                glDeleteTextures(1, &frame.tid);
            }
        }
    }

    if (mSwitching && !exitPending()) {
        return true;
    } else {
        return false;
    }
}

// ---------------------------------------------------------------------------

}
; // namespace android
