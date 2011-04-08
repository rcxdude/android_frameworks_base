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
#include <math.h>
#include <fcntl.h>
#include <utils/misc.h>
#include <signal.h>

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
    LOGD("hellloooo");
    char *line;
    initLogDevice();
    if (getLogLine(&line)) {
        LOGD("first line read: %s",line);
        delete line;
    }
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
            LOGD("1");
            break;
        case SkBitmap::kARGB_4444_Config:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                    GL_UNSIGNED_SHORT_4_4_4_4, p);
            LOGD("2");
            break;
        case SkBitmap::kARGB_8888_Config:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, p);
            LOGD("3");
            break;
        case SkBitmap::kRGB_565_Config:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB,
                    GL_UNSIGNED_SHORT_5_6_5, p);
            LOGD("4");
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

    return NO_ERROR;
}

bool BootAnimation::threadLoop()
{
    bool r;
    if (mAndroidAnimation) {
        r = android();
    } else {
        r = movie();
    }

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
    glTranslatef(-1,1,0);
    //glTranslatex(-1<<16,1<<16,0);
    glScalef(2.0/mWidth,-2.0/mHeight,1);
    //glScalex((2<<16)/(mWidth<<16),(-2<<16)/(mHeight<<16),1<<16);
    float xoffset = 0;
    float yoffset = 0;
    //GLfixed xoffset = 0;
    //GLfixed yoffset = 0;
    mFontWidth = 10;
    mFontHeight = 18;
    float fontWidthOffset = float(mFontWidth)/(float)mFontTex.w;
    float fontHeightOffset = float(mFontHeight)/(float)mFontTex.h;
    //GLfixed fontWidthOffset = (mFontWidth << 16)/(mFontTex.w << 16);
    //GLfixed fontHeightOffset = (mFontHeight << 16)/(mFontTex.h << 16);
    mCols = mWidth / mFontWidth;
    mRows = mHeight / mFontHeight;
    for (int x = 0;x<96;x++)
    {
        xoffset = (float)(x%24) * fontWidthOffset;
        yoffset = (float)(x/24) * fontHeightOffset;
        //xoffset = (x%24) * fontWidthOffset;
        //yoffset = (x/24) * fontHeightOffset;
        mFontTexCoords[x*8] = xoffset;
        mFontTexCoords[x*8+1] = yoffset;

        mFontTexCoords[x*8+2] = xoffset;
        mFontTexCoords[x*8+3] = yoffset + fontHeightOffset;

        mFontTexCoords[x*8+4] = xoffset + fontWidthOffset;
        mFontTexCoords[x*8+5] = yoffset;

        mFontTexCoords[x*8+6] = xoffset + fontWidthOffset;
        mFontTexCoords[x*8+7] = yoffset + fontHeightOffset;

        mFontVert[x*8] = 0;
        mFontVert[x*8+1] = 0;

        mFontVert[x*8+2] = 0;
        mFontVert[x*8+3] = mFontHeight;

        mFontVert[x*8+4] = mFontWidth;
        mFontVert[x*8+5] = 0;

        mFontVert[x*8+6] = mFontWidth;
        mFontVert[x*8+7] = mFontHeight;
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
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

    printLine("Hello to you too!");
    printLine("Hello to you too!");
    printLine("this is very long texthis is very long texthis is very long texthis is very long texthis is very long texthis is very long texthis is very long texthis is very long texttttttttthis is very long text");
    printLine("Hello to you too!");
    return true;
}

void BootAnimation::printLine(char *s) {
    int col = 0;
    char c;
    while ((c = *s++))
    {
        mLineBuffer[mBufferPos][col++] = c;
        if (col > mCols) {
            col = 0;
            mBufferPos = (mBufferPos + 1) % mRows;
        }
    }
    for (;col<mCols;col++) {
        mLineBuffer[mBufferPos][col] = '\0';
    }
    mBufferPos = (mBufferPos + 1) % mRows;
}

void BootAnimation::drawText() {
    glVertexPointer(2,GL_SHORT,0,mFontVert);
    glTexCoordPointer(2,GL_FLOAT,0,mFontTexCoords);
    //glTexCoordPointer(2,GL_FIXED,0,mFontTexCoords);
    glBindTexture(GL_TEXTURE_2D, mFontTex.name);
    for (int i = 0; i<mRows; i++) {
        drawLine(mLineBuffer[(mBufferPos + i) % mRows],i);
    }
    return;
}

void BootAnimation::drawLine(char *s,int line)
{
    char c = 0;
    glPushMatrix();
    //glTranslatex(0,(mFontHeight * line) << 16,0);
    glTranslatef(0,(mFontHeight * line),0);
    while ((c = *s++))
    {
        c -= 32;
        if (c<96)
        {
            glDrawArrays(GL_TRIANGLE_STRIP,c*4,4);
            glTranslatex(mFontWidth << 16 ,0,0);
        }
    }
    glPopMatrix();

    return;
}

bool BootAnimation::initLogDevice() {
    char devname[] = "/dev/"LOGGER_LOG_MAIN;
    mLogDevice = open(devname,O_RDONLY|O_NONBLOCK);
    if (mLogDevice < 0) {
        LOGE("unable to open log device %s, %s",devname,strerror(errno));
        return false;
    }
    return true;
}

bool BootAnimation::getLogLine(char** s)
{
    if (mLogDevice < 0) {
        return false;
        LOGE("log device not open");
    }
    union pun{
        unsigned char buf[LOGGER_ENTRY_MAX_LEN + 1] __attribute__((aligned(4)));
        struct logger_entry entry __attribute__((aligned(4)));
    };
    union pun rdbuf;
    int ret;
    ret = read(mLogDevice,rdbuf.buf,LOGGER_ENTRY_MAX_LEN);
    if (ret < 0) {
        if (errno == EAGAIN) {
            return false;
        } else {
            LOGE("unable to read from log device %d, %s",mLogDevice,strerror(errno));
        }
        return false;
    }
    if (ret == 0) {
        LOGE("unexpected EOF on device %d!",mLogDevice);
        return false;
    }
    AndroidLogEntry processedEntry;
    if ( 0 != android_log_processLogBuffer(&rdbuf.entry,&processedEntry) ) {
        LOGE("error processing log buffer");
        return false;
    }
    char *retstr = new char[processedEntry.messageLen + 1];
    strncpy(retstr,processedEntry.message,processedEntry.messageLen);
    *s = retstr;
    return true;
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
    //mFlingerSurface->setSwapRectangle(updateRect);

    glScissor(updateRect.left, mHeight - updateRect.bottom, updateRect.width(),
            updateRect.height());

    // Blend state
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glTexEnvx(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    initFont();
    initBuffer();

    const nsecs_t startTime = systemTime();
    do {
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

        glDisable(GL_SCISSOR_TEST);
        drawText();

        EGLBoolean res = eglSwapBuffers(mDisplay, mSurface);
        if (res == EGL_FALSE)
            break;

        // 12fps: don't animate too fast to preserve CPU
        const nsecs_t sleepTime = 83333 - ns2us(systemTime() - now);
        if (sleepTime > 0)
            usleep(sleepTime);
    } while (!exitPending());

    glDeleteTextures(1, &mAndroid[0].name);
    glDeleteTextures(1, &mAndroid[1].name);
    glDeleteTextures(1, &mAndroid[2].name);
    glDeleteTextures(1, &mFontTex.name);
    return false;
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

    initFont();
    initBuffer();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (int i=0 ; i<pcount && !exitPending() ; i++) {
        const Animation::Part& part(animation.parts[i]);
        const size_t fcount = part.frames.size();
        glBindTexture(GL_TEXTURE_2D, 0);

        for (int r=0 ; !part.count || r<part.count ; r++) {
            for (int j=0 ; j<fcount && !exitPending(); j++) {
                char* message;
                while (getLogLine(&message)) {
                    printLine(message);
                    delete message;
                }
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
                glDisable(GL_SCISSOR_TEST);
                glEnable(GL_BLEND);
                drawText();
                glDisable(GL_BLEND);
                glBindTexture(GL_TEXTURE_2D, 0);
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
    glDeleteTextures(1, &mFontTex.name);

    return false;
}

// ---------------------------------------------------------------------------

}
; // namespace android
