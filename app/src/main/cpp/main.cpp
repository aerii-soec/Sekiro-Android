#include "assets/bnd4.h"
#include "assets/flver.h"
#include "vulkan_renderer.h"

#include <android/log.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
constexpr char logTag[] = "SekiroNative";
constexpr int64_t maxAssetBytes = 64 * 1024 * 1024;

std::mutex windowInsetsMutex;
std::array<uint32_t, 4> windowInsets{};
ALooper* insetLooper = nullptr;

struct Application {
    std::optional<sekiro::assets::Texture> texture;
    std::u16string textureStem;
    std::vector<sekiro::assets::ModelTexture> bundledTextures;
    std::vector<sekiro::assets::ModelTextureView> materialViews;
    std::optional<sekiro::assets::FlverModel> model;
    std::unique_ptr<VulkanRenderer> renderer;
    bool resumed = false;
    bool focused = false;
    bool redraw = true;
    bool failed = false;
    std::array<uint32_t, 4> cutout{};

    bool canDraw() const { return renderer && resumed && focused && redraw; }
};

std::vector<uint8_t> readLocalAsset(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return {};
    }
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

std::vector<uint8_t> readBundledAsset(AAssetManager* manager, const char* path) {
    std::unique_ptr<AAsset, decltype(&AAsset_close)> asset(
        AAssetManager_open(manager, path, AASSET_MODE_STREAMING), &AAsset_close);
    if (!asset) {
        return {};
    }
    const auto length = AAsset_getLength64(asset.get());
    if (length <= 0 || length > maxAssetBytes) {
        throw std::runtime_error(std::string("Bundled asset exceeds size limits: ") + path);
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = AAsset_read(asset.get(), bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) {
            throw std::runtime_error(std::string("Could not read bundled asset: ") + path);
        }
        offset += static_cast<size_t>(count);
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

sekiro::assets::FlverModel readPreviewModel(std::span<const uint8_t> bytes) {
    if (bytes.size() >= 4 && std::equal(bytes.begin(), bytes.begin() + 4, "BND4")) {
        const auto members = sekiro::assets::readBnd4(bytes);
        for (const auto& member : members) {
            if (!member.compressed && member.path.ends_with(u".flver")) {
                return sekiro::assets::readFlver2(bytes.subspan(member.offset, member.storedBytes));
            }
        }
        throw std::runtime_error("The BND4 has no uncompressed FLVER member. Open an exported FLVER file.");
    }
    return sekiro::assets::readFlver2(bytes);
}

void loadInspectionAssets(ANativeActivity& activity, Application& state) {
    const char* filesDirectory = activity.internalDataPath;
    if (!filesDirectory) {
        reportIssue(activity, "The Android app files directory is unavailable.");
        return;
    }
    const std::filesystem::path directory(filesDirectory);
    __android_log_print(ANDROID_LOG_INFO, logTag, "Native activity files: %s", filesDirectory);
    try {
        const auto binder = readLocalAsset(directory / "inspection.bnd");
        if (!binder.empty()) {
            const auto members = sekiro::assets::readBnd4(binder);
            __android_log_print(ANDROID_LOG_INFO, logTag, "Read %zu BND4 members", members.size());
        }
    } catch (const std::exception& error) {
        __android_log_print(ANDROID_LOG_WARN, logTag, "BND4: %s", error.what());
    }

    std::string assetError;
    bool bundledModel = false;
    try {
        if (!std::filesystem::exists(directory / "texture-only")) {
            auto bytes = readLocalAsset(directory / "inspection.flver");
            if (bytes.empty()) {
                bytes = readBundledAsset(activity.assetManager, "sekiro/model.flver");
                bundledModel = !bytes.empty();
            }
            if (!bytes.empty()) {
                auto model = readPreviewModel(bytes);
                if (bundledModel) {
                    const auto materials = readBundledAsset(activity.assetManager, "sekiro/materials.skmp");
                    if (!materials.empty()) {
                        state.bundledTextures = sekiro::assets::readModelMaterials(materials, model);
                        for (const auto& texture : state.bundledTextures) {
                            state.materialViews.push_back({texture.stem, &texture.image});
                        }
                    }
                }
                state.model = std::move(model);
                __android_log_print(ANDROID_LOG_INFO, logTag,
                    "Loaded %s FLVER (%zu bytes), %zu material records, %zu bones, %zu bundled textures",
                    bundledModel ? "bundled" : "private", bytes.size(), state.model->materials.size(),
                    state.model->bones.size(), state.bundledTextures.size());
            }
        }
    } catch (const std::exception& error) {
        assetError = std::string("Could not load the model. ") + error.what();
        __android_log_write(ANDROID_LOG_ERROR, logTag, assetError.c_str());
    }
    try {
        // The bundled model already has its own original material set.
        if (!state.model || !bundledModel) {
            auto bytes = readLocalAsset(directory / "inspection.tpf");
            const bool bundledTexture = bytes.empty();
            if (bundledTexture) {
                bytes = readBundledAsset(activity.assetManager, "sekiro/preview.tpf");
            }
            if (!bytes.empty()) {
                state.texture = sekiro::assets::readTpfBc1(bytes);
                state.textureStem = sekiro::assets::assetStem(sekiro::assets::tpfTextureName(bytes));
                if (state.model && !state.textureStem.empty()) {
                    state.materialViews.push_back({state.textureStem, &*state.texture});
                }
                __android_log_print(ANDROID_LOG_INFO, logTag, "Loaded %s TPF (%zu bytes); decoded %ux%u BC1 sRGB mip 0",
                                    bundledTexture ? "bundled" : "private", bytes.size(),
                                    state.texture->width, state.texture->height);
            }
        }
    } catch (const std::exception& error) {
        if (assetError.empty()) {
            assetError = std::string("Could not decode the TPF. ") + error.what();
        }
        __android_log_print(ANDROID_LOG_ERROR, logTag, "TPF: %s", error.what());
    }
    if (!assetError.empty()) {
        reportIssue(activity, assetError.c_str());
    } else if (!state.model && !state.texture) {
        __android_log_write(ANDROID_LOG_INFO, logTag, "No local preview assets; APK ships shaders without game assets");
        reportIssue(activity, "Open a TPF texture or FLVER model from your local Sekiro files.");
    }
}

void fail(android_app& app, Application& state, const char* error) {
    __android_log_print(ANDROID_LOG_ERROR, logTag, "%s", error);
    state.renderer.reset();
    state.failed = true;
    const std::string message = std::string("Vulkan preview failed. ") + error;
    reportIssue(*app.activity, message.c_str());
}

void updateCutout(Application& state) {
    const std::scoped_lock lock(windowInsetsMutex);
    if (state.cutout != windowInsets) {
        state.cutout = windowInsets;
        __android_log_print(ANDROID_LOG_INFO, logTag, "Display cutout insets %u,%u,%u,%u",
                            windowInsets[0], windowInsets[1], windowInsets[2], windowInsets[3]);
        if (state.renderer) {
            state.renderer->setCutout(state.cutout);
        }
        state.redraw = true;
    }
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
                                           state.texture ? &*state.texture : nullptr,
                                           state.model ? &*state.model : nullptr, state.materialViews);
                state.renderer->setCutout(state.cutout);
                state.redraw = true;
            }
            break;
        case APP_CMD_TERM_WINDOW:
            __android_log_write(ANDROID_LOG_INFO, logTag, "Native window terminated");
            state.renderer.reset();
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONTENT_RECT_CHANGED:
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
    {
        const std::scoped_lock lock(windowInsetsMutex);
        insetLooper = app->looper;
    }
    loadInspectionAssets(*app->activity, state);
    while (!app->destroyRequested) {
        void* data = nullptr;
        const int events = ALooper_pollOnce(state.canDraw() ? 16 : -1, nullptr, nullptr, &data);
        updateCutout(state);
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
    const std::scoped_lock lock(windowInsetsMutex);
    if (insetLooper == app->looper) {
        insetLooper = nullptr;
        windowInsets = {};
    }
}

extern "C" JNIEXPORT void JNICALL
Java_dev_sekiro_nativeport_PreviewActivity_nativeSetCutout(JNIEnv*, jclass, jint left, jint top,
                                                         jint right, jint bottom) {
    const std::array<uint32_t, 4> insets{static_cast<uint32_t>(left), static_cast<uint32_t>(top),
                                         static_cast<uint32_t>(right), static_cast<uint32_t>(bottom)};
    const std::scoped_lock lock(windowInsetsMutex);
    if (windowInsets != insets) {
        windowInsets = insets;
        if (insetLooper) {
            ALooper_wake(insetLooper);
        }
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_sekiro_nativeport_PreviewActivity_validateAsset(JNIEnv* env, jclass, jstring path, jboolean model) {
    const char* text = env->GetStringUTFChars(path, nullptr);
    if (!text) {
        return nullptr;
    }
    const auto release = [&](const char* value) { env->ReleaseStringUTFChars(path, value); };
    const std::unique_ptr<const char, decltype(release)> input(text, release);
    try {
        const auto bytes = readLocalAsset(input.get());
        if (model) {
            (void)readPreviewModel(bytes);
        } else {
            (void)sekiro::assets::readTpfBc1(bytes);
        }
        return nullptr;
    } catch (const std::exception& error) {
        return env->NewStringUTF(error.what());
    }
}
