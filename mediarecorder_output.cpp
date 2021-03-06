#include "screenrec.h"
#include "mediarecorder_output.h"

using namespace android;


void AbstractMediaRecorderOutput::setupOutput() {
    outputFd = open(outputName, O_RDWR | O_CREAT, 0744);
    if (outputFd < 0 && fixOutputName()) {
        outputFd = open(outputName, O_RDWR | O_CREAT, 0744);
    }
    if (outputFd < 0) {
        stop(201, "Could not open the output file");
    }
    if (audioSource != SCR_AUDIO_MUTE) {
        checkAudioSource(AUDIO_SOURCE_MIC);
        checkAudioSource(AUDIO_SOURCE_VOICE_COMMUNICATION);
        checkAudioSource(AUDIO_SOURCE_CAMCORDER);
    }
}

void AbstractMediaRecorderOutput::checkAudioSource(audio_source_t source) {
    // Checking if source is active by starting AudioRecord turned out to cause 213 errors on some systems using ALSA
    // so for now check only on 4.2+ using AudioSystem::isSourceActive
    #if SCR_SDK_VERSION >= 17
    ALOGV("Checking if audio source is available");
    status_t err = OK;
    int64_t startTime = getTimeMs();
    bool active = false;
    err = AudioSystem::isSourceActive(source, &active);
    if (err != NO_ERROR) {
        ALOGE("Error when checking audio source state %#x (%d)", err, err);
    } else if (active) {
        ALOGE("audio source already active: %d", source);
        stop(251, "audio source already active");
    }
    ALOGV("audio check time %lldms", getTimeMs() - startTime);
    #endif
}

// Set up the MediaRecorder which runs in the same process as mediaserver
void AbstractMediaRecorderOutput::setupMediaRecorder() {
    #if SCR_SDK_VERSION >= 23
    mr = new MediaRecorder(String16("com.iwobanas.screenrecorder.pro"));
    #else
    mr = new MediaRecorder();
    #endif
    if (mr->initCheck() != NO_ERROR) {
        stop(231, "Error starting MediaRecorder");
    }
    sp<SCRListener> listener = new SCRListener();
    mr->setListener(listener);
    #if SCR_SDK_VERSION >= 21
    mr->setVideoSource(VIDEO_SOURCE_SURFACE);
    #else
    mr->setVideoSource(VIDEO_SOURCE_GRALLOC_BUFFER);
    #endif
    if (audioSource != SCR_AUDIO_MUTE) {
        mr->setAudioSource(AUDIO_SOURCE_MIC);
    }
    mr->setOutputFormat(OUTPUT_FORMAT_MPEG_4);
    mr->setVideoEncoder(videoEncoder);
    if (audioSource != SCR_AUDIO_MUTE) {
        mr->setAudioEncoder(AUDIO_ENCODER_AAC);
        mr->setParameters(String8::format("audio-param-sampling-rate=%d", audioSamplingRate));
        mr->setParameters(String8::format("audio-param-number-of-channels=%d", audioChannels));
        mr->setParameters(String8("audio-param-encoding-bitrate=128000"));
    }
    mr->setOutputFile(outputFd, 0, 0);
    mr->setVideoSize(videoWidth, videoHeight);
    mr->setVideoFrameRate(frameRate);
    mr->setParameters(String8::format("video-param-rotation-angle-degrees=%d", rotation));
    if (videoBitrate > 0) {
        mr->setParameters(String8::format("video-param-encoding-bitrate=%d", videoBitrate));
    }
    setFileSizeLimit();
    mr->prepare();

    ALOGV("Starting MediaRecorder...");
    if (mr->start() != OK) {
        stop(213, "Error starting MediaRecorder");
        return;
    } else {
        mrRunning = true;
    }

    //TODO: check media recorder status
    #if SCR_SDK_VERSION >= 18
    sp<IGraphicBufferProducer> iST = mr->querySurfaceMediaSourceFromMediaServer();
    #else
    sp<ISurfaceTexture> iST = mr->querySurfaceMediaSourceFromMediaServer();
    #endif // SCR_SDK_VERSION
    if (iST.get() == NULL) {
        mrRunning = false;
        stop(198, "error starting MediaRecorder");
        return;
    }
    mSTC = new Surface(iST);
    mANW = mSTC;

    int format = PIXEL_FORMAT_RGBA_8888;
    if (useYUV_P) {
        format = HAL_PIXEL_FORMAT_YV12;
    } else if (useYUV_SP) {
        #if SCR_SDK_VERSION < 18
        format = HAL_PIXEL_FORMAT_YCrCb_420_SP;
        #else
        format = HAL_PIXEL_FORMAT_YCbCr_420_888;
        #endif
    }
    if (native_window_set_buffers_format(mANW.get(), format) != NO_ERROR) {
        stop(225, "native_window_set_buffers_format");
    }
}

void AbstractMediaRecorderOutput::setFileSizeLimit() {
    uint64_t space = getAvailableSpace();
    if (space > 0) {
        if (space > 100*1024*1024) {
            space -= 50*1024*1024;
        } else if (space > 1*1024*1024) {
            space -= 1*1024*1024;
            ALOGW("Low storage space %llukB", space / 1024);
        } else {
            ALOGE("Low storage space %llukB or space measurement failed", space / 1024);
        }
        ALOGV("Setting file size limit to %lluMB", space / (1024 * 1024));
        if (space > USE_64BIT_OFFSET_LIMIT) {
            mr->setParameters(String8("param-use-64bit-offset=1"));
        }
        mr->setParameters(String8::format("max-filesize=%llu", space));
    }
}

void AbstractMediaRecorderOutput::closeOutput(bool fromMainThread) {
    tearDownMediaRecorder(fromMainThread);

    if (outputFd >= 0) {
         close(outputFd);
         outputFd = -1;
    }
    ALOGV("Abstract output closed");
}


void AbstractMediaRecorderOutput::tearDownMediaRecorder(bool async) {
    if (mr.get() != NULL) {
        if (mrRunning) {
            if (async) {
                stopMediaRecorderAsync();
            } else {
                stopMediaRecorder();
            }
        }
        mr.clear();
    }
}


void AbstractMediaRecorderOutput::stopMediaRecorderAsync() {
    bool waitForStoppingThread = !videoSourceError;

    // MediaRecorder needs to be stopped from separate thread as couple frames may need to be rendered before mr->stop() returns.
    if (pthread_create(&stoppingThread, NULL, &AbstractMediaRecorderOutput::stoppingThreadStart, (void *)this) != 0){
        ALOGE("Can't create stopping thread, stopping synchronously");
        stopMediaRecorder();
    }
    while (mrRunning && !videoSourceError) {
        renderFrame();
    }
    if (waitForStoppingThread) {
        ALOGV("Waiting for stopping thread");
        pthread_join(stoppingThread, NULL);
        ALOGV("Stopping thread finished");
    } else {
        ALOGW("Not waiting for stopping thread to finish");
    }
}


void AbstractMediaRecorderOutput::stopMediaRecorder() {
    if (mr.get() != NULL) {
        ALOGV("Stopping MediaRecorder");
        mr->stop();
        mrRunning = false;
        ALOGV("MediaRecorder Stopped");
    }
}


void* AbstractMediaRecorderOutput::stoppingThreadStart(void* args) {
    ALOGV("stoppingThreadStart");
    AbstractMediaRecorderOutput* output = static_cast<AbstractMediaRecorderOutput*>(args);
    output->stopMediaRecorder();
    return NULL;
}

uint64_t AbstractMediaRecorderOutput::getAvailableSpace() {
    struct statfs stats;
    int err = statfs(outputName, &stats);
    if (err != NO_ERROR) {
        ALOGW("Can't retrieve free storage space %s %d", strerror(errno), err);
        return 0;
    }
    uint64_t space = stats.f_bavail * uint64_t(stats.f_bsize);
    if (stats.f_type == MSDOS_SUPER_MAGIC && space > MSDOS_FS_LIMIT) {
        ALOGV("File size limit restricted to 4GiB because of filesystem limitations");
        return MSDOS_FS_LIMIT;
    }
    return space;
}



static const char sVertexShader[] =
    "attribute vec4 vPosition;\n"
    "attribute vec2 texCoord; \n"
    "uniform mat4 vTransform;"
    "varying vec2 tc; \n"
    "void main() {\n"
    "  tc = texCoord;\n"
    "  gl_Position = vTransform * vPosition;\n"
    "}\n";

static const char sFragmentShader[] =
    "precision mediump float;\n"
    "uniform sampler2D textureSampler; \n"
    "uniform mat4 colorTransform;"
    "varying vec2 tc; \n"
    "void main() {\n"
    "  gl_FragColor.rgba = colorTransform * texture2D(textureSampler, tc); \n"
    "}\n";


static const char sFragmentShaderOES[] =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES textureSampler; \n"
    "uniform mat4 colorTransform;"
    "varying vec2 tc; \n"
    "void main() {\n"
    "  gl_FragColor.rgba = colorTransform * texture2D(textureSampler, tc); \n"
    "}\n";

static EGLint eglConfigAttribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_RECORDABLE_ANDROID, EGL_TRUE,
            EGL_NONE };

static EGLint eglContextAttribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE };

GLfloat GLMediaRecorderOutput::flipAndRotateMatrix[16] = {
    0.0, 1.0, 0.0, 0.0,
    1.0, 0.0, 0.0, 0.0,
    0.0, 0.0,-1.0, 0.0,
    0.0, 0.0, 0.0, 1};

GLfloat GLMediaRecorderOutput::flipMatrix[16] = {
    1.0, 0.0, 0.0, 0.0,
    0.0,-1.0, 0.0, 0.0,
    0.0, 0.0,-1.0, 0.0,
    0.0, 0.0, 0.0, 1};

GLfloat GLMediaRecorderOutput::rgbaMatrix[16] = {
    1.0, 0.0, 0.0, 0.0,
    0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0};

GLfloat GLMediaRecorderOutput::bgraMatrix[16] = {
    0.0, 0.0, 1.0, 0.0,
    0.0, 1.0, 0.0, 0.0,
    1.0, 0.0, 0.0, 0.0,
    0.0, 0.0, 0.0, 1.0};

void GLMediaRecorderOutput::setupOutput() {
    AbstractMediaRecorderOutput::setupOutput();
    setupEgl();
    setupMediaRecorder();
    if (!stopping) {
        setupGl();
    }
}


void GLMediaRecorderOutput::setupEgl() {
    ALOGV("setupEgl()");
    mEglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglGetError() != EGL_SUCCESS || mEglDisplay == EGL_NO_DISPLAY) {
        stop(207, "eglGetDisplay() failed");
    }

    EGLint majorVersion;
    EGLint minorVersion;
    eglInitialize(mEglDisplay, &majorVersion, &minorVersion);
    if (eglGetError() != EGL_SUCCESS) {
        stop(208, "eglInitialize() failed");
    }

    EGLint numConfigs = 0;
    eglChooseConfig(mEglDisplay, eglConfigAttribs, &mEglconfig, 1, &numConfigs);
    if (eglGetError() != EGL_SUCCESS  || numConfigs < 1) {
        stop(209, "eglChooseConfig() failed");
    }
    mEglContext = eglCreateContext(mEglDisplay, mEglconfig, EGL_NO_CONTEXT, eglContextAttribs);
    if (eglGetError() != EGL_SUCCESS || mEglContext == EGL_NO_CONTEXT) {
        stop(210, "eglGetDisplay() failed");
    }
    ALOGV("EGL initialized");
}


void GLMediaRecorderOutput::setupGl() {
    ALOGV("setup GL");

    mEglSurface = eglCreateWindowSurface(mEglDisplay, mEglconfig, mANW.get(), NULL);
    if (eglGetError() != EGL_SUCCESS || mEglSurface == EGL_NO_SURFACE) {
        stop(214, "eglCreateWindowSurface() failed");
    };

    eglMakeCurrent(mEglDisplay, mEglSurface, mEglSurface, mEglContext);
    if (eglGetError() != EGL_SUCCESS ) {
        stop(215, "eglMakeCurrent() failed");
    };

    transformMatrix = rotateView ? flipAndRotateMatrix : flipMatrix;

    if (useOes) {
        mProgram = createProgram(sVertexShader, sFragmentShaderOES);
    } else {
        mProgram = createProgram(sVertexShader, sFragmentShader);
    }
    if (!mProgram) {
        stop(212, "Could not create GL program.");
    }

    mvPositionHandle = glGetAttribLocation(mProgram, "vPosition");
    mTexCoordHandle = glGetAttribLocation(mProgram, "texCoord");
    checkGlError("glGetAttribLocation");

    mvTransformHandle = glGetUniformLocation(mProgram, "vTransform");
    checkGlError("glGetUniformLocation");

    mColorTransformHandle = glGetUniformLocation(mProgram, "colorTransform");
    checkGlError("glGetUniformLocation");

    if (useOes) {
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, 1);
        checkGlError("glBindTexture");

        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        checkGlError("glTexParameteri");

        texCoordinates[3]  = 1;
        texCoordinates[7]  = 1;
        texCoordinates[9]  = 1;
        texCoordinates[10] = 1;
    } else {
        glDeleteTextures(1, &mTexture);
        glGenTextures(1, &mTexture);
        glBindTexture(GL_TEXTURE_2D, mTexture);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        checkGlError("texture setup");

        int texWidth = getTexSize(inputStride);
        int texHeight = getTexSize(inputHeight);

        mPixels = (uint32_t*)malloc(4 * texWidth * texHeight);
        if (mPixels == (uint32_t*)NULL) {
            stop(211, "malloc failed");
        }

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texWidth, texHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, mPixels);
        checkGlError("glTexImage2D", true);
        
        GLfloat wTexPortion = inputWidth/(float)texWidth;
        GLfloat hTexPortion = inputHeight/(float)texHeight;

        texCoordinates[3] = wTexPortion;
        texCoordinates[7] = hTexPortion;
        texCoordinates[9] = wTexPortion;
        texCoordinates[10] = hTexPortion;
    }

    GLfloat wVideoPortion = (GLfloat) (videoWidth - 2 * paddingWidth) / (GLfloat) videoWidth;
    GLfloat hVideoPortion = (GLfloat) (videoHeight - 2 * paddingHeight) / (GLfloat) videoHeight;

    //TODO: use GL uniforms instead of updating matrices
    if (rotateView) {
        GLfloat tmp = wVideoPortion;
        wVideoPortion = hVideoPortion;
        hVideoPortion = tmp;
    }
    vertices[0] = -wVideoPortion;
    vertices[3] =  wVideoPortion;
    vertices[6] = -wVideoPortion;
    vertices[9] =  wVideoPortion;
    vertices[1] = -hVideoPortion;
    vertices[4] = -hVideoPortion;
    vertices[7] =  hVideoPortion;
    vertices[10]=  hVideoPortion;

    colorMatrix = useBGRA ? bgraMatrix : rgbaMatrix;

    glViewport(0, 0, videoWidth, videoHeight);
    checkGlError("glViewport");
}


int GLMediaRecorderOutput::getTexSize(int size) {
    int texSize = 2;
    while (texSize < size) {
        texSize = texSize * 2;
    }
    return texSize;
}



void GLMediaRecorderOutput::renderFrame() {
    if (videoSourceError) return;
    updateInput();

    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!useOes && inputBase != NULL) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, inputStride, inputHeight, GL_RGBA, GL_UNSIGNED_BYTE, inputBase);
        checkGlError("glTexSubImage2D");
    }

    glUseProgram(mProgram);
    checkGlError("glUseProgram");

    glUniformMatrix4fv(mvTransformHandle, 1, GL_FALSE, transformMatrix);
    checkGlError("glUniformMatrix4fv");

    glUniformMatrix4fv(mColorTransformHandle, 1, GL_FALSE, colorMatrix);
    checkGlError("glUniformMatrix4fv");

    glVertexAttribPointer(mvPositionHandle, 3, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(mvPositionHandle);
    glVertexAttribPointer(mTexCoordHandle, 3, GL_FLOAT, GL_FALSE, 0, texCoordinates);
    glEnableVertexAttribArray(mTexCoordHandle);
    checkGlError("vertexAttrib");

    updateTexImage();

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    checkGlError("glDrawArrays");

    if (mrRunning) {
        eglSwapBuffers(mEglDisplay, mEglSurface);
        if (eglGetError() != EGL_SUCCESS) {
            videoSourceError = true;
            if (!stopping) {
                stop(243, "eglSwapBuffers failed");
            }
        }
    }
}


void GLMediaRecorderOutput::closeOutput(bool fromMainThread) {
    AbstractMediaRecorderOutput::closeOutput(fromMainThread);
    tearDownEgl();
    ALOGV("Output closed");
}


void GLMediaRecorderOutput::tearDownEgl() {
    if (mEglContext != EGL_NO_CONTEXT) {
        eglDestroyContext(mEglDisplay, mEglContext);
        mEglContext = EGL_NO_CONTEXT;
    }
    if (mEglSurface != EGL_NO_SURFACE) {
        eglDestroySurface(mEglDisplay, mEglSurface);
        mEglSurface = EGL_NO_SURFACE;
    }
    if (mEglDisplay != EGL_NO_DISPLAY) {
        eglTerminate(mEglDisplay);
        mEglDisplay = EGL_NO_DISPLAY;
    }
    if (eglGetError() != EGL_SUCCESS) {
        ALOGE("tearDownEgl() failed");
    }
}


void CPUMediaRecorderOutput::setupOutput() {
    AbstractMediaRecorderOutput::setupOutput();
    setupMediaRecorder();
    if (!stopping) {
        #if SCR_SDK_VERSION < 17
        if (native_window_api_connect(mANW.get(), NATIVE_WINDOW_API_CPU) != NO_ERROR) {
            stop(224, "native_window_api_connect");
        }
        #endif
    }
    ALOGV("Output closed");
}


void CPUMediaRecorderOutput::renderFrame() {
    if (videoSourceError) return;
    updateInput();

    ANativeWindowBuffer* anb;
    int rv;

    #if SCR_SDK_VERSION > 16
    rv = native_window_set_buffers_user_dimensions(mANW.get(), videoWidth, videoHeight);
    #else
    rv = native_window_set_buffers_dimensions(mANW.get(), videoWidth, videoHeight);
    #endif

    if (rv != NO_ERROR) {
        stop(241, "native_window_set_buffers_user_dimensions");
    }

    #if SCR_SDK_VERSION > 16
    native_window_set_scaling_mode(mANW.get(), NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
    #endif

    #if SCR_SDK_VERSION > 16
    rv = native_window_dequeue_buffer_and_wait(mANW.get(), &anb);
    #else
    rv = mANW->dequeueBuffer(mANW.get(), &anb);
    #endif

    if (rv != NO_ERROR || anb == NULL) {
        videoSourceError = true;

        if (!stopping) {
            stop(242, "dequeueBuffer failed");
        }
        return;
    }

    sp<GraphicBuffer> buf(new GraphicBuffer(anb, false));

    #if SCR_SDK_VERSION < 17
    mANW->lockBuffer(mANW.get(), buf->getNativeBuffer());
    #endif

    if (inputBase != NULL) {
        fillBuffer(buf);
    }

    #if SCR_SDK_VERSION > 16
    rv = mANW->queueBuffer(mANW.get(), buf->getNativeBuffer(), -1);
    #else
    rv = mANW->queueBuffer(mANW.get(), buf->getNativeBuffer());
    #endif

    if (rv != NO_ERROR) {
        videoSourceError = true;
        if (!stopping) {
            stop(245, "queueBuffer failed");
        }
    }
}

void CPUMediaRecorderOutput::fillBuffer(sp<GraphicBuffer> buf) {
    uint32_t* bufPixels = NULL;
    uint32_t* screen = (uint32_t*) inputBase;
    status_t rv = buf->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, (void**)(&bufPixels));

    if (rv != NO_ERROR) {
        if (stopping) return;
        stop(233, "buf->lock");
    }

    int stride = buf->stride;

    if (rotateView) {
        if (useYUV_P || useYUV_SP) {
            copyRotateYUVBuf((uint8_t*) bufPixels, (uint8_t*) screen, stride);
        } else {
            copyRotateBuf(bufPixels, screen, stride);
        }
    } else {
        if (useYUV_P || useYUV_SP) {
            copyYUVBuf((uint8_t*) bufPixels, (uint8_t*) screen, stride);
        } else {
            if (stride == inputStride && !useBGRA && paddingWidth == 0 && paddingHeight == 0) {
                memcpy(bufPixels, screen, stride * videoHeight * 4);
            } else {
                copyBuf(bufPixels, screen, stride);
            }
        }
    }

    buf->unlock();
}

void CPUMediaRecorderOutput::copyRotateBuf(uint32_t* bufPixels, uint32_t* screen, int stride) {
    for (int y = paddingHeight; y < videoHeight - paddingHeight; y++) {
        for (int x = paddingWidth; x < videoWidth - paddingWidth; x++) {
            uint32_t color = screen[(x - paddingWidth) * inputStride + videoHeight - paddingHeight - y - 1];
            bufPixels[y * stride + x] = convertColor(color);
        }
    }
}
void CPUMediaRecorderOutput::copyBuf(uint32_t* bufPixels, uint32_t* screen, int stride) {
    //TODO: test on some device with this screen orientation
    for (int y = paddingHeight; y < videoHeight - paddingHeight; y++) {
        for (int x = paddingWidth; x < videoWidth - paddingWidth; x++) {
            uint32_t color = screen[(y - paddingHeight) * inputStride + (x - paddingWidth)];
            bufPixels[y * stride + x] = convertColor(color);
        }
    }
}

inline uint32_t CPUMediaRecorderOutput::convertColor(uint32_t color) {
    if (useBGRA) {
        return (color & 0xFF00FF00) | ((color >> 16) & 0x000000FF) | ((color << 16) & 0x00FF0000);
    }
    return color;
}

void CPUMediaRecorderOutput::copyRotateYUVBuf(uint8_t* yuvPixels, uint8_t* screen, int stride) {
    for (int x = paddingWidth; x < videoWidth - paddingWidth; x++) {
        for (int y = videoHeight - paddingHeight - 1; y >= paddingHeight; y--) {
            int idx = ((x - paddingWidth) * inputStride + videoHeight - paddingHeight - y - 1) * 4;
            uint8_t r,g,b;
            if (useBGRA) {
                b = screen[idx];
                g = screen[idx + 1];
                r = screen[idx + 2];
            } else {
                r = screen[idx];
                g = screen[idx + 1];
                b = screen[idx + 2];
            }
            uint16_t Y = ( (  66 * r + 129 * g +  25 * b + 128) >> 8) +  16;
            uint16_t U = ( ( -38 * r -  74 * g + 112 * b + 128) >> 8) + 128;
            uint16_t V = ( ( 112 * r -  94 * g -  18 * b + 128) >> 8) + 128;
            yuvPixels[y * stride + x] = Y;
            if (y % 2 == 0 && x % 2 == 0) {
                if (useYUV_P) {
                    yuvPixels[videoHeight * stride + y * stride / 4 + x / 2 ] = U;
                    yuvPixels[videoHeight * stride + videoHeight * stride / 4 + y * stride / 4 + x / 2 ] = V;
                } else { // useYUV_SP
                    yuvPixels[videoHeight * stride + y * stride / 2 + x ] = U;
                    yuvPixels[videoHeight * stride + y * stride / 2 + x + 1] = V;
                }
            }
        }
    }
}

void CPUMediaRecorderOutput::copyYUVBuf(uint8_t* yuvPixels, uint8_t* screen, int stride) {
    for (int y = paddingHeight; y < videoHeight - paddingHeight; y++) {
        for (int x = paddingWidth; x < videoWidth - paddingWidth; x++) {

            int idx = ((y - paddingHeight) * inputStride + x - paddingWidth) * 4;
            uint8_t r,g,b;
            if (useBGRA) {
                b = screen[idx];
                g = screen[idx + 1];
                r = screen[idx + 2];
            } else {
                r = screen[idx];
                g = screen[idx + 1];
                b = screen[idx + 2];
            }
            uint16_t Y = ( (  66 * r + 129 * g +  25 * b + 128) >> 8) +  16;
            yuvPixels[y * stride + x] = Y;
            if (y % 2 == 0 && x % 2 == 0) {
                uint16_t U = ( ( -38 * r -  74 * g + 112 * b + 128) >> 8) + 128;
                uint16_t V = ( ( 112 * r -  94 * g -  18 * b + 128) >> 8) + 128;
                if (useYUV_P) {
                    yuvPixels[videoHeight * stride + y * stride / 4 + x / 2 ] = U;
                    yuvPixels[videoHeight * stride + videoHeight * stride / 4 + y * stride / 4 + x / 2 ] = V;
                } else { // useYUV_SP
                    yuvPixels[videoHeight * stride + y * stride / 2 + x ] = U;
                    yuvPixels[videoHeight * stride + y * stride / 2 + x + 1] = V;
                }
            }
        }
    }
}


void CPUMediaRecorderOutput::closeOutput(bool fromMainThread) {
    AbstractMediaRecorderOutput::closeOutput(fromMainThread);
    if (mANW.get() != NULL) {
        #if SCR_SDK_VERSION < 17
        native_window_api_disconnect(mANW.get(), NATIVE_WINDOW_API_CPU);
        #endif
    }
}


void SCRListener::notify(int msg, int ext1, int ext2)
{
    int trackId = (ext1 >> 28);
    int errorInfo = (ext1 & 0x0000FFFF);
    ALOGI("SCRListener %d %d %d, track: %d value: %d\n", msg, ext1, ext2, trackId, errorInfo);

    int error = 0;

    if (msg == MEDIA_RECORDER_EVENT_ERROR) {
        ALOGE("MEDIA_RECORDER_EVENT_ERROR");
        if (mrRunning) {
            error = 227;
            // stop(227, "MEDIA_RECORDER_EVENT_ERROR");
        } else {
            error = 248;
            // stop(248, "MEDIA_RECORDER_EVENT_ERROR during startup");
        }
    } else if (msg == MEDIA_RECORDER_TRACK_EVENT_ERROR) {
        ALOGE("MEDIA_RECORDER_TRACK_EVENT_ERROR");
        if (mrRunning) {
            if (trackId == 2) {
                error = 197;
                // stop(197, "MEDIA_RECORDER_TRACK_EVENT_ERROR - audio");
            } else {
                error = 228;
                // stop(228, "MEDIA_RECORDER_TRACK_EVENT_ERROR - video");
            }
        } else {
            error = 249;
            // stop(249, "MEDIA_RECORDER_TRACK_EVENT_ERROR during startup");
        }
    } else if (msg == MEDIA_RECORDER_EVENT_INFO && ext1 == MEDIA_RECORDER_INFO_MAX_FILESIZE_REACHED) {
        ALOGE("MEDIA_RECORDER_INFO_MAX_FILESIZE_REACHED");
        error = 229;
        // stop(229, "MEDIA_RECORDER_INFO_MAX_FILESIZE_REACHED");
    } else if (msg == MEDIA_RECORDER_EVENT_INFO && ext1 == MEDIA_RECORDER_INFO_MAX_DURATION_REACHED) {
        ALOGE("MEDIA_RECORDER_INFO_MAX_DURATION_REACHED");
        error = 230;
        // stop(230, "MEDIA_RECORDER_INFO_MAX_DURATION_REACHED");
    }

    if (error != 0 && firstError) { //TODO: add proper thread handling instead of this hack
        firstError = false;

        usleep(500); // wait a moment for mediaserver to finish it's stuff

        stop(error, false, "stopping from listener");

        ALOGV("SCRListener thread completed");
    }
}

// OpenGL helpers

void GLMediaRecorderOutput::checkGlError(const char* op, bool critical) {
    for (GLint error = glGetError(); error; error
            = glGetError()) {
        ALOGI("after %s() glError (0x%x)\n", op, error);
        if (critical) {
            stop(218, op);
            // stop(218, "glGetError returned error");
        }
    }
}

void GLMediaRecorderOutput::checkGlError(const char* op) {
    checkGlError(op, false);
}

GLuint GLMediaRecorderOutput::loadShader(GLenum shaderType, const char* pSource) {
    GLuint shader = glCreateShader(shaderType);
    if (shader) {
        glShaderSource(shader, 1, &pSource, NULL);
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            GLint infoLen = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
            if (infoLen) {
                char* buf = (char*) malloc(infoLen);
                if (buf) {
                    glGetShaderInfoLog(shader, infoLen, NULL, buf);
                    ALOGE("Could not compile shader %d:\n%s\n",
                            shaderType, buf);
                    free(buf);
                }
                glDeleteShader(shader);
                shader = 0;
            }
        }
    }
    return shader;
}

GLuint GLMediaRecorderOutput::createProgram(const char* pVertexSource, const char* pFragmentSource) {
    GLuint vertexShader = loadShader(GL_VERTEX_SHADER, pVertexSource);
    if (!vertexShader) {
        return 0;
    }

    GLuint pixelShader = loadShader(GL_FRAGMENT_SHADER, pFragmentSource);
    if (!pixelShader) {
        return 0;
    }

    GLuint program = glCreateProgram();
    if (program) {
        glAttachShader(program, vertexShader);
        checkGlError("glAttachShader");
        glAttachShader(program, pixelShader);
        checkGlError("glAttachShader");
        glLinkProgram(program);
        GLint linkStatus = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
        if (linkStatus != GL_TRUE) {
            GLint bufLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bufLength);
            if (bufLength) {
                char* buf = (char*) malloc(bufLength);
                if (buf) {
                    glGetProgramInfoLog(program, bufLength, NULL, buf);
                    ALOGE("Could not link program:\n%s\n", buf);
                    free(buf);
                }
            }
            glDeleteProgram(program);
            program = 0;
        }
    }
    return program;
}
