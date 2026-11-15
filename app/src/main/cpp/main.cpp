#include "assets/bnd4.h"
#include "vulkan_renderer.h"

#include <android/log.h>
#include <android_native_app_glue.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
constexpr char logTag[] = "SekiroNative";

struct Application {
    std::optional<sekiro::assets::Texture> texture;
    std::unique_ptr<VulkanRenderer> renderer;
    bool resumed = false;
    bool focused = false;
    bool redraw = true;
    bool failed = false;

    bool canDraw() const { return renderer && resumed && focused && redraw; }
};

std::vector<uint8_t> readLocalAsset(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return {};
    }
    constexpr std::streamoff maxAssetBytes = 64 * 1024 * 1024;
    const auto size = input.tellg();
    if (size <= 0 || size > maxAssetBytes) {
        throw std::runtime_error("Inspection asset exceeds size limits");
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("Could not read inspection asset");
    }
    return bytes;
}

void reportIssue(ANativeActivity& activity, const char* message) {
    JNIEnv* env = nullptr;
    if (activity.vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        __android_log_write(ANDROID_LOG_ERROR, logTag, "Could not attach native thread for preview status");
        return;
    }
    const jclass type = env->GetObjectClass(activity.clazz);
    if (type) {
        const jmethodID notify = env->GetMethodID(type, "onPreviewIssue", "(Ljava/lang/String;)V");
        if (notify) {
            const jstring text = env->NewStringUTF(message);
            if (text) {
                env->CallVoidMethod(activity.clazz, notify, text);
                env->DeleteLocalRef(text);
            }
        }
        env->DeleteLocalRef(type);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    activity.vm->DetachCurrentThread();
}

void loadInspectionAssets(ANativeActivity& activity, Application& state) {
    const char* filesDirectory = activity.internalDataPath;
    __android_log_print(ANDROID_LOG_INFO, logTag, "Native activity started; private files directory: %s",
                        filesDirectory ? filesDirectory : "(unavailable)");
    if (!filesDirectory) {
        reportIssue(activity, "The Android app files directory is unavailable.");
        return;
    }
    const std::filesystem::path directory(filesDirectory);
    try {
        const auto binder = readLocalAsset(directory / "inspection.bnd");
        if (!binder.empty()) {
            const auto members = sekiro::assets::readBnd4(binder);
            __android_log_print(ANDROID_LOG_INFO, logTag, "Read %zu BND4 members", members.size());
        }
    } catch (const std::exception& error) {
        __android_log_print(ANDROID_LOG_WARN, logTag, "BND4: %s", error.what());
    }

    try {
        const auto path = directory / "inspection.tpf";
        const auto texture = readLocalAsset(path);
        if (texture.empty()) {
            __android_log_print(ANDROID_LOG_WARN, logTag,
                                "No readable texture at %s; APK ships shaders without game assets", path.c_str());
            reportIssue(activity, "Open inspection.tpf from your local Sekiro files to preview the texture.");
            return;
        }
        state.texture = sekiro::assets::readTpfBc1(texture);
        __android_log_print(ANDROID_LOG_INFO, logTag, "Loaded private inspection.tpf (%zu bytes); decoded %ux%u BC1 sRGB mip 0",
                            texture.size(), state.texture->width, state.texture->height);
    } catch (const std::exception& error) {
        __android_log_print(ANDROID_LOG_ERROR, logTag, "TPF load failed: %s", error.what());
        const std::string message = std::string("Could not decode inspection.tpf. ") + error.what();
        reportIssue(activity, message.c_str());
    }
}


void fail(android_app& app, Application& state, const char* error) {
    __android_log_print(ANDROID_LOG_ERROR, logTag, "%s", error);
    state.renderer.reset();
    state.failed = true;
    const std::string message = std::string("Vulkan preview failed. ") + error;
    reportIssue(*app.activity, message.c_str());
}

void onCommand(android_app* app, int32_t command) {
    auto& state = *static_cast<Application*>(app->userData);
    try {
        switch (command) {
        case APP_CMD_INIT_WINDOW:
            if (app->window && !state.failed) {
                state.renderer = std::make_unique<VulkanRenderer>();
                __android_log_print(ANDROID_LOG_INFO, logTag, "Native window initialized (%dx%d)",
                                    ANativeWindow_getWidth(app->window), ANativeWindow_getHeight(app->window));
                state.renderer->initialize(app->window, app->activity->assetManager,
                                           state.texture ? &*state.texture : nullptr);
                state.redraw = true;
            }
            break;
        case APP_CMD_TERM_WINDOW:
            __android_log_write(ANDROID_LOG_INFO, logTag, "Native window terminated");
            state.renderer.reset();
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
            if (state.renderer) {
                state.renderer->invalidateSwapchain();
                state.redraw = true;
            }
            break;
        case APP_CMD_WINDOW_REDRAW_NEEDED:
            if (state.renderer) {
                state.redraw = !state.renderer->draw();
            }
            break;
        case APP_CMD_RESUME:
            state.resumed = true;
            state.redraw = true;
            break;
        case APP_CMD_PAUSE:
            state.resumed = false;
            break;
        case APP_CMD_GAINED_FOCUS:
            state.focused = true;
            break;
        case APP_CMD_LOST_FOCUS:
            state.focused = false;
            break;
        }
    } catch (const std::exception& error) {
        fail(*app, state, error.what());
    }
}
}

void android_main(android_app* app) {
    Application state;
    app->userData = &state;
    app->onAppCmd = onCommand;
    loadInspectionAssets(*app->activity, state);
    while (!app->destroyRequested) {
        void* data = nullptr;
        const int events = ALooper_pollOnce(state.canDraw() ? 16 : -1, nullptr, nullptr, &data);
        if (data) {
            auto* source = static_cast<android_poll_source*>(data);
            source->process(app, source);
        }
        if (events == ALOOPER_POLL_ERROR && !state.failed) {
            fail(*app, state, "Android looper poll failed");
        }
        if (!app->destroyRequested && state.canDraw()) {
            try {
                state.redraw = !state.renderer->draw();
            } catch (const std::exception& error) {
                fail(*app, state, error.what());
            }
        }
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_sekiro_nativeport_PreviewActivity_validateTexture(JNIEnv* env, jclass, jstring path) {
    const char* text = env->GetStringUTFChars(path, nullptr);
    if (!text) {
        return nullptr;
    }
    const auto release = [&](const char* value) { env->ReleaseStringUTFChars(path, value); };
    const std::unique_ptr<const char, decltype(release)> input(text, release);
    try {
        (void)sekiro::assets::readTpfBc1(readLocalAsset(input.get()));
        return nullptr;
    } catch (const std::exception& error) {
        return env->NewStringUTF(error.what());
    }
}