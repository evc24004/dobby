#include "ui/entity_hitbox_overlay.hpp"
#include "ui/chest_esp.hpp"
#include "ui/ore_esp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <numeric>
#include <utility>
#include <vector>

namespace dobby {
namespace {

constexpr float kNearPlane = 0.01F;
constexpr float kCompactMarkerMaximumSize = 8.0F;
constexpr std::size_t kMaximumBoxesPerFrame = 512;

Vec3f subtract(const Vec3f& left, const Vec3f& right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

bool finite(const Vec3f& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool validBounds(const EntityAabb& bounds) {
    if (!finite(bounds.minimum) || !finite(bounds.maximum))
        return false;
    const Vec3f extent = subtract(bounds.maximum, bounds.minimum);
    return extent.x > 0.0001F && extent.y > 0.0001F && extent.z > 0.0001F &&
            extent.x <= 1024.0F && extent.y <= 1024.0F && extent.z <= 1024.0F;
}

bool validCamera(const CameraFrame& camera) {
    constexpr float maximumWorldCoordinate = 1.0e8F;
    if (!finite(camera.position) ||
        std::fabs(camera.position.x) >= maximumWorldCoordinate ||
        std::fabs(camera.position.y) >= maximumWorldCoordinate ||
        std::fabs(camera.position.z) >= maximumWorldCoordinate) {
        return false;
    }
    const auto finiteMatrix = [](const std::array<float, 16>& matrix) {
        return std::all_of(matrix.begin(), matrix.end(), [](float value) {
            return std::isfinite(value) && std::fabs(value) < 1.0e8F;
        });
    };
    const float viewMagnitude = std::accumulate(
            camera.view.begin(), camera.view.end(), 0.0F,
            [](float total, float value) { return total + std::fabs(value); });
    return finiteMatrix(camera.view) && finiteMatrix(camera.projection) &&
            viewMagnitude > 0.01F &&
            std::fabs(camera.projection[0]) > 0.0001F &&
            std::fabs(camera.projection[5]) > 0.0001F;
}

std::array<float, 4> transform(
        const std::array<float, 16>& matrix, const std::array<float, 4>& point) {
    return {{
            point[0] * matrix[0] + point[1] * matrix[4] +
                    point[2] * matrix[8] + point[3] * matrix[12],
            point[0] * matrix[1] + point[1] * matrix[5] +
                    point[2] * matrix[9] + point[3] * matrix[13],
            point[0] * matrix[2] + point[1] * matrix[6] +
                    point[2] * matrix[10] + point[3] * matrix[14],
            point[0] * matrix[3] + point[1] * matrix[7] +
                    point[2] * matrix[11] + point[3] * matrix[15],
    }};
}

std::array<float, 4> worldToClip(
        const CameraFrame& camera, const Vec3f& world) {
    const Vec3f relative = subtract(world, camera.position);
    const auto eye = transform(
            camera.view, {{relative.x, relative.y, relative.z, 1.0F}});
    return transform(camera.projection, eye);
}

bool finiteClip(const std::array<float, 4>& clip) {
    return std::all_of(clip.begin(), clip.end(), [](float value) {
        return std::isfinite(value);
    });
}

bool clipToScreen(
        const std::array<float, 4>& clip, float width, float height,
        ScreenPoint& output) {
    if (!finiteClip(clip) || clip[3] < kNearPlane)
        return false;
    const float normalizedX = clip[0] / clip[3];
    const float normalizedY = clip[1] / clip[3];
    if (!std::isfinite(normalizedX) || !std::isfinite(normalizedY))
        return false;
    output.x = (normalizedX + 1.0F) * width * 0.5F;
    output.y = (1.0F - normalizedY) * height * 0.5F;
    return std::isfinite(output.x) && std::isfinite(output.y);
}

bool projectClipSegment(
        std::array<float, 4> firstClip, std::array<float, 4> secondClip,
        float width, float height, ScreenPoint& firstOutput,
        ScreenPoint& secondOutput, bool& nearPlaneClipped) {
    nearPlaneClipped = false;
    if (!finiteClip(firstClip) || !finiteClip(secondClip) ||
        (firstClip[3] <= kNearPlane && secondClip[3] <= kNearPlane)) {
        return false;
    }
    const auto clipEndpoint = [&](std::array<float, 4>& behind,
                                  const std::array<float, 4>& ahead) {
        const float denominator = ahead[3] - behind[3];
        if (!std::isfinite(denominator) || denominator <= 0.0F)
            return false;
        const float amount = (kNearPlane - behind[3]) / denominator;
        if (!std::isfinite(amount) || amount < 0.0F || amount > 1.0F)
            return false;
        for (std::size_t index = 0; index < behind.size(); ++index)
            behind[index] += (ahead[index] - behind[index]) * amount;
        behind[3] = kNearPlane;
        nearPlaneClipped = true;
        return true;
    };
    if (firstClip[3] <= kNearPlane && !clipEndpoint(firstClip, secondClip))
        return false;
    if (secondClip[3] <= kNearPlane && !clipEndpoint(secondClip, firstClip))
        return false;
    return clipToScreen(firstClip, width, height, firstOutput) &&
            clipToScreen(secondClip, width, height, secondOutput);
}

struct CapturedBox {
    EntityAabb bounds;
    std::uint64_t lastSeenFrame{};
};

std::mutex captureMutex;
std::vector<CapturedBox> capturedBoxes;
std::uintptr_t capturedLevel{};
CameraFrame latestCamera{};
std::uintptr_t latestCameraLevel{};
std::uint64_t latestCameraFrame{};
std::atomic_uint64_t presentationFrame{};

} // namespace

bool validCameraFrame(const CameraFrame& camera) {
    return validCamera(camera);
}

bool validEntityBounds(const EntityAabb& bounds) {
    return validBounds(bounds);
}

bool projectWorldPoint(
        const CameraFrame& camera, const Vec3f& world, float width, float height,
        ScreenPoint& output) {
    if (!validCamera(camera) || !finite(world) || !std::isfinite(width) ||
        !std::isfinite(height) || width <= 0.0F || height <= 0.0F) {
        return false;
    }

    return clipToScreen(worldToClip(camera, world), width, height, output);
}

bool projectWorldSegment(
        const CameraFrame& camera, const Vec3f& firstWorld,
        const Vec3f& secondWorld, float width, float height,
        ScreenPoint& firstOutput, ScreenPoint& secondOutput,
        bool& nearPlaneClipped) {
    nearPlaneClipped = false;
    if (!validCamera(camera) || !finite(firstWorld) || !finite(secondWorld) ||
        !std::isfinite(width) || !std::isfinite(height) || width <= 0.0F ||
        height <= 0.0F) {
        return false;
    }

    return projectClipSegment(
            worldToClip(camera, firstWorld), worldToClip(camera, secondWorld),
            width, height, firstOutput, secondOutput, nearPlaneClipped);
}

bool worldPointWithinViewport(
        const CameraFrame& camera, const Vec3f& world,
        float normalizedMargin) {
    if (!validCamera(camera) || !finite(world) ||
        !std::isfinite(normalizedMargin) || normalizedMargin < 1.0F) {
        return false;
    }
    const auto clip = worldToClip(camera, world);
    if (!finiteClip(clip) || clip[3] < kNearPlane)
        return false;
    const float horizontal = std::fabs(clip[0] / clip[3]);
    const float vertical = std::fabs(clip[1] / clip[3]);
    return std::isfinite(horizontal) && std::isfinite(vertical) &&
            horizontal <= normalizedMargin && vertical <= normalizedMargin;
}

bool shouldUseCompactEspMarker(float widthPixels, float heightPixels) {
    return std::isfinite(widthPixels) && std::isfinite(heightPixels) &&
            widthPixels >= 0.0F && heightPixels >= 0.0F &&
            widthPixels <= kCompactMarkerMaximumSize &&
            heightPixels <= kCompactMarkerMaximumSize;
}

HitboxFrameSubmission submitEntityHitboxFrame(
        const void* levelIdentity,
        std::span<const EntityHitboxObservation> observations) {
    HitboxFrameSubmission result{};
    if (levelIdentity == nullptr) {
        result.invalid = observations.size();
        return result;
    }
    std::lock_guard lock(captureMutex);
    const auto level = reinterpret_cast<std::uintptr_t>(levelIdentity);
    if (capturedLevel != level) {
        capturedBoxes.clear();
        capturedLevel = level;
    }
    capturedBoxes.clear();
    capturedBoxes.reserve(kMaximumBoxesPerFrame);
    const std::uint64_t frame = presentationFrame.load(std::memory_order_relaxed);
    for (const EntityHitboxObservation& observation : observations) {
        if (observation.identity == nullptr || !validBounds(observation.bounds)) {
            ++result.invalid;
            continue;
        }
        if (capturedBoxes.size() >= kMaximumBoxesPerFrame) {
            ++result.capacityRejected;
            continue;
        }
        capturedBoxes.push_back({observation.bounds, frame});
        ++result.accepted;
    }
    return result;
}

bool submitOverlayCameraFrame(
        const void* levelIdentity, const CameraFrame& camera) {
    if (levelIdentity == nullptr || !validCamera(camera))
        return false;
    std::lock_guard lock(captureMutex);
    latestCamera = camera;
    latestCameraLevel = reinterpret_cast<std::uintptr_t>(levelIdentity);
    latestCameraFrame = presentationFrame.load(std::memory_order_relaxed);
    return true;
}

bool currentOverlayCamera(
        std::uint64_t currentFrame, const void*& levelIdentity,
        CameraFrame& camera, std::uint64_t* missedFrames) {
    std::lock_guard lock(captureMutex);
    if (missedFrames != nullptr)
        *missedFrames = 0;
    if (latestCameraLevel == 0 || currentFrame < latestCameraFrame)
        return false;
    if (missedFrames != nullptr)
        *missedFrames = currentFrame - latestCameraFrame;
    if (!entityHitboxObservedForPresentation(currentFrame, latestCameraFrame))
        return false;
    levelIdentity = reinterpret_cast<const void*>(latestCameraLevel);
    camera = latestCamera;
    return true;
}

std::uint64_t entityHitboxPresentationFrame() {
    return presentationFrame.load(std::memory_order_acquire);
}

bool entityHitboxObservedForPresentation(
        std::uint64_t currentFrame, std::uint64_t lastSeenFrame) {
    return currentFrame >= lastSeenFrame &&
            currentFrame - lastSeenFrame <=
                    kMaximumMissedEntityPresentationFrames;
}

} // namespace dobby

#if defined(__ANDROID__)

#include "core/runtime_state.hpp"
#include "hooks/network_metrics_hook.hpp"
#include "hooks/ore_esp_scanner.hpp"
#include "hooks/overlay_camera_hook.hpp"
#include "metrics/client_performance.hpp"
#include "metrics/network_metrics.hpp"
#include "metrics/packet_traffic.hpp"
#include "platform/launcher.hpp"
#include "platform/log.hpp"
#include "ui/network_metrics_overlay.hpp"
#include "ui/packet_traffic_overlay.hpp"

#include <GLES2/gl2.h>

#include <atomic>
#include <cstdio>
#include <dlfcn.h>
#include <limits>

namespace dobby {
namespace {

constexpr std::size_t kMaximumOresPerFrame = 4'096;

struct GlApi {
    PFNGLCREATESHADERPROC createShader{};
    PFNGLSHADERSOURCEPROC shaderSource{};
    PFNGLCOMPILESHADERPROC compileShader{};
    PFNGLGETSHADERIVPROC getShaderiv{};
    PFNGLGETSHADERINFOLOGPROC getShaderInfoLog{};
    PFNGLDELETESHADERPROC deleteShader{};
    PFNGLCREATEPROGRAMPROC createProgram{};
    PFNGLATTACHSHADERPROC attachShader{};
    PFNGLBINDATTRIBLOCATIONPROC bindAttribLocation{};
    PFNGLLINKPROGRAMPROC linkProgram{};
    PFNGLGETPROGRAMIVPROC getProgramiv{};
    PFNGLGETPROGRAMINFOLOGPROC getProgramInfoLog{};
    PFNGLDELETEPROGRAMPROC deleteProgram{};
    PFNGLGENBUFFERSPROC genBuffers{};
    PFNGLBINDBUFFERPROC bindBuffer{};
    PFNGLBUFFERDATAPROC bufferData{};
    PFNGLENABLEVERTEXATTRIBARRAYPROC enableVertexAttribArray{};
    PFNGLDISABLEVERTEXATTRIBARRAYPROC disableVertexAttribArray{};
    PFNGLGETVERTEXATTRIBIVPROC getVertexAttribiv{};
    PFNGLVERTEXATTRIBPOINTERPROC vertexAttribPointer{};
    PFNGLUSEPROGRAMPROC useProgram{};
    PFNGLGETUNIFORMLOCATIONPROC getUniformLocation{};
    PFNGLUNIFORM4FPROC uniform4f{};
    PFNGLGETINTEGERVPROC getIntegerv{};
    PFNGLGETFLOATVPROC getFloatv{};
    PFNGLISENABLEDPROC isEnabled{};
    PFNGLENABLEPROC enable{};
    PFNGLDISABLEPROC disable{};
    PFNGLBLENDFUNCSEPARATEPROC blendFuncSeparate{};
    PFNGLLINEWIDTHPROC lineWidth{};
    PFNGLDRAWARRAYSPROC drawArrays{};
    PFNGLBINDFRAMEBUFFERPROC bindFramebuffer{};
    PFNGLCHECKFRAMEBUFFERSTATUSPROC checkFramebufferStatus{};
    PFNGLVIEWPORTPROC viewport{};
    PFNGLGETERRORPROC getError{};
    PFNGLGETSTRINGPROC getString{};
};

GlApi gl;
GLuint program{};
GLuint vertexBuffer{};
GLint colorUniform{-1};
std::atomic_bool overlayInstalled{false};
std::atomic_bool failedProjectionLogged{false};
std::atomic_bool successfulProjectionLogged{false};
std::atomic_bool presentationSampleLogged{false};
std::atomic_bool rendererFailureLogged{false};
std::atomic_bool chestPresentationLogged{false};
std::atomic_bool chestLevelMismatchLogged{false};
std::atomic_bool orePresentationLogged{false};
std::atomic_bool oreLevelMismatchLogged{false};
std::atomic_bool oreCameraStale{false};
std::atomic_bool nearPlaneClipLogged{false};
std::atomic_bool synchronizedCameraLogged{false};
std::atomic_bool captureGapActive{false};
std::atomic_bool cameraExpiredLogged{false};
std::size_t largestBatchLogged = 0;
std::size_t batchSamplesLogged = 0;

template <class Function>
bool resolveGlFunction(void* library, Function& output, const char* name) {
    void* symbol = library == nullptr ? nullptr : dlsym(library, name);
    output = reinterpret_cast<Function>(symbol);
    if (output == nullptr) {
        char message[160]{};
        std::snprintf(message, sizeof(message),
                      "ERROR: entity overlay missing GLES2 function %s", name);
        logLine(message);
    }
    return output != nullptr;
}

bool resolveGlApi() {
    void* library = dlopen("libGLESv2.so", RTLD_NOW | RTLD_NOLOAD);
    if (library == nullptr)
        library = dlopen("libGLESv2.so", RTLD_NOW);
    return resolveGlFunction(library, gl.createShader, "glCreateShader") &&
            resolveGlFunction(library, gl.shaderSource, "glShaderSource") &&
            resolveGlFunction(library, gl.compileShader, "glCompileShader") &&
            resolveGlFunction(library, gl.getShaderiv, "glGetShaderiv") &&
            resolveGlFunction(library, gl.getShaderInfoLog, "glGetShaderInfoLog") &&
            resolveGlFunction(library, gl.deleteShader, "glDeleteShader") &&
            resolveGlFunction(library, gl.createProgram, "glCreateProgram") &&
            resolveGlFunction(library, gl.attachShader, "glAttachShader") &&
            resolveGlFunction(library, gl.bindAttribLocation, "glBindAttribLocation") &&
            resolveGlFunction(library, gl.linkProgram, "glLinkProgram") &&
            resolveGlFunction(library, gl.getProgramiv, "glGetProgramiv") &&
            resolveGlFunction(library, gl.getProgramInfoLog, "glGetProgramInfoLog") &&
            resolveGlFunction(library, gl.deleteProgram, "glDeleteProgram") &&
            resolveGlFunction(library, gl.genBuffers, "glGenBuffers") &&
            resolveGlFunction(library, gl.bindBuffer, "glBindBuffer") &&
            resolveGlFunction(library, gl.bufferData, "glBufferData") &&
            resolveGlFunction(library, gl.enableVertexAttribArray, "glEnableVertexAttribArray") &&
            resolveGlFunction(library, gl.disableVertexAttribArray, "glDisableVertexAttribArray") &&
            resolveGlFunction(library, gl.getVertexAttribiv, "glGetVertexAttribiv") &&
            resolveGlFunction(library, gl.vertexAttribPointer, "glVertexAttribPointer") &&
            resolveGlFunction(library, gl.useProgram, "glUseProgram") &&
            resolveGlFunction(library, gl.getUniformLocation, "glGetUniformLocation") &&
            resolveGlFunction(library, gl.uniform4f, "glUniform4f") &&
            resolveGlFunction(library, gl.getIntegerv, "glGetIntegerv") &&
            resolveGlFunction(library, gl.getFloatv, "glGetFloatv") &&
            resolveGlFunction(library, gl.isEnabled, "glIsEnabled") &&
            resolveGlFunction(library, gl.enable, "glEnable") &&
            resolveGlFunction(library, gl.disable, "glDisable") &&
            resolveGlFunction(library, gl.blendFuncSeparate, "glBlendFuncSeparate") &&
            resolveGlFunction(library, gl.lineWidth, "glLineWidth") &&
            resolveGlFunction(library, gl.drawArrays, "glDrawArrays") &&
            resolveGlFunction(library, gl.bindFramebuffer, "glBindFramebuffer") &&
            resolveGlFunction(library, gl.checkFramebufferStatus, "glCheckFramebufferStatus") &&
            resolveGlFunction(library, gl.viewport, "glViewport") &&
            resolveGlFunction(library, gl.getError, "glGetError") &&
            resolveGlFunction(library, gl.getString, "glGetString");
}

GLuint compileShader(GLenum type, const char* source) {
    const GLuint shader = gl.createShader(type);
    if (shader == 0)
        return 0;
    gl.shaderSource(shader, 1, &source, nullptr);
    gl.compileShader(shader);
    GLint compiled = GL_FALSE;
    gl.getShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
        return shader;
    char details[512]{};
    GLsizei length = 0;
    gl.getShaderInfoLog(shader, static_cast<GLsizei>(sizeof(details) - 1), &length, details);
    char message[640]{};
    std::snprintf(message, sizeof(message),
                  "ERROR: entity overlay shader compile failed: %s", details);
    logLine(message);
    gl.deleteShader(shader);
    return 0;
}

bool createRenderer() {
    constexpr char vertexSource[] =
            "#version 100\n"
            "attribute vec2 position;\n"
            "void main(){ gl_Position=vec4(position,0.0,1.0); }\n";
    constexpr char fragmentSource[] =
            "#version 100\n"
            "precision mediump float;\n"
            "uniform vec4 color;\n"
            "void main(){ gl_FragColor=color; }\n";
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (vertex == 0 || fragment == 0) {
        if (vertex != 0)
            gl.deleteShader(vertex);
        if (fragment != 0)
            gl.deleteShader(fragment);
        return false;
    }
    program = gl.createProgram();
    gl.attachShader(program, vertex);
    gl.attachShader(program, fragment);
    gl.bindAttribLocation(program, 0, "position");
    gl.linkProgram(program);
    gl.deleteShader(vertex);
    gl.deleteShader(fragment);
    GLint linked = GL_FALSE;
    gl.getProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char details[512]{};
        GLsizei length = 0;
        gl.getProgramInfoLog(
                program, static_cast<GLsizei>(sizeof(details) - 1), &length, details);
        char message[640]{};
        std::snprintf(message, sizeof(message),
                      "ERROR: entity overlay program link failed: %s", details);
        logLine(message);
        gl.deleteProgram(program);
        program = 0;
        return false;
    }
    colorUniform = gl.getUniformLocation(program, "color");
    gl.genBuffers(1, &vertexBuffer);
    return colorUniform >= 0 && vertexBuffer != 0;
}

std::array<Vec3f, 8> corners(const EntityAabb& bounds) {
    return {{
            {bounds.minimum.x, bounds.minimum.y, bounds.minimum.z},
            {bounds.maximum.x, bounds.minimum.y, bounds.minimum.z},
            {bounds.maximum.x, bounds.maximum.y, bounds.minimum.z},
            {bounds.minimum.x, bounds.maximum.y, bounds.minimum.z},
            {bounds.minimum.x, bounds.minimum.y, bounds.maximum.z},
            {bounds.maximum.x, bounds.minimum.y, bounds.maximum.z},
            {bounds.maximum.x, bounds.maximum.y, bounds.maximum.z},
            {bounds.minimum.x, bounds.maximum.y, bounds.maximum.z},
    }};
}

void restoreCapability(GLenum capability, bool enabled) {
    if (enabled)
        gl.enable(capability);
    else
        gl.disable(capability);
}

void clearGlErrors() {
    for (int count = 0; count < 16 && gl.getError() != GL_NO_ERROR; ++count) {
    }
}

void drawLines(const float* vertices, std::size_t floatCount,
               float red, float green, float blue, float alpha, float width) {
    if (vertices == nullptr || floatCount < 4)
        return;
    gl.bufferData(
            GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(floatCount * sizeof(float)),
            vertices, GL_STREAM_DRAW);
    gl.uniform4f(colorUniform, red, green, blue, alpha);
    gl.lineWidth(width);
    gl.drawArrays(GL_LINES, 0, static_cast<GLsizei>(floatCount / 2));
}

void drawEntityHitboxes(void*, void* display, void* surface) {
    const bool espEnabled = runtimeState().anyEspEnabled();
    const bool metricsEnabled = runtimeState().networkMetricsOverlay();
    const bool worldRecentlyRendered = clientWorldRecentlyRendered();
    const bool packetTrafficEnabled = runtimeState().packetTrafficOverlay() &&
            runtimeState().packetTrafficAvailable() &&
            worldRecentlyRendered;
    if (!espEnabled && !metricsEnabled && !packetTrafficEnabled)
        return;

    const bool showHitboxes = runtimeState().entityHitboxes();
    const bool showChests = runtimeState().chestEsp();
    const bool showOres = runtimeState().oreEsp();
    if (metricsEnabled)
        captureObservedClientServerTick();
    NetworkMetricsSnapshot metrics = metricsEnabled
            ? currentNetworkMetrics() : NetworkMetricsSnapshot{};
    // A live client-world render is the authoritative connection signal for
    // presentation. Keep every diagnostic row visible while individual hooks
    // are still waiting for their first sample instead of silently reducing
    // the overlay to FPS and memory.
    if (metricsEnabled && worldRecentlyRendered)
        metrics.connected = true;
    const ClientPerformanceSnapshot performance = metricsEnabled
            ? captureClientPerformance() : ClientPerformanceSnapshot{};
    const PacketTrafficSnapshot packetTraffic = packetTrafficEnabled
            ? currentPacketTraffic() : PacketTrafficSnapshot{};
    thread_local std::vector<CapturedBox> boxes;
    boxes.clear();
    boxes.reserve(kMaximumBoxesPerFrame);
    std::uintptr_t boxLevel = 0;
    const std::uint64_t currentFrame =
            presentationFrame.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (showHitboxes) {
        std::lock_guard lock(captureMutex);
        for (auto entry = capturedBoxes.begin(); entry != capturedBoxes.end();) {
            if (!entityHitboxObservedForPresentation(
                        currentFrame, entry->lastSeenFrame)) {
                entry = capturedBoxes.erase(entry);
            } else {
                boxes.push_back(*entry);
                ++entry;
            }
        }
        boxLevel = capturedLevel;
    }
    const void* cameraLevel = nullptr;
    CameraFrame overlayCamera{};
    std::uint64_t missedCameraFrames = 0;
    const bool currentCameraAvailable =
            (showHitboxes || showChests || showOres) && currentOverlayCamera(
                    currentFrame, cameraLevel, overlayCamera,
                    &missedCameraFrames);
    if (showOres && !currentCameraAvailable &&
        !oreCameraStale.exchange(true, std::memory_order_acq_rel)) {
        char message[144]{};
        std::snprintf(
                message, sizeof(message),
                "ore ESP state: camera unavailable (snapshot age=%llu frame(s))",
                static_cast<unsigned long long>(missedCameraFrames));
        logLine(message);
    } else if (showOres && currentCameraAvailable &&
               oreCameraStale.exchange(false, std::memory_order_acq_rel)) {
        logLine("ore ESP state: camera capture recovered");
    }
    if (currentCameraAvailable && missedCameraFrames > 1 &&
        !captureGapActive.exchange(true, std::memory_order_acq_rel)) {
        char message[144]{};
        std::snprintf(
                message, sizeof(message),
                "entity overlay: retaining snapshot across %llu missed "
                "render-camera frame(s)",
                static_cast<unsigned long long>(missedCameraFrames - 1));
        verboseLine(message);
    } else if (!currentCameraAvailable &&
               missedCameraFrames > kMaximumMissedEntityPresentationFrames &&
               !cameraExpiredLogged.exchange(true, std::memory_order_acq_rel)) {
        char message[144]{};
        std::snprintf(
                message, sizeof(message),
                "ERROR: entity overlay expired after %llu frame(s) without "
                "a render-camera callback",
                static_cast<unsigned long long>(missedCameraFrames - 1));
        verboseLine(message);
    } else if (currentCameraAvailable && missedCameraFrames <= 1 &&
               captureGapActive.exchange(false, std::memory_order_acq_rel)) {
        verboseLine("entity overlay: render-camera capture recovered");
        cameraExpiredLogged.store(false, std::memory_order_release);
    }
    if (!showHitboxes || !currentCameraAvailable ||
        boxLevel != reinterpret_cast<std::uintptr_t>(cameraLevel)) {
        boxes.clear();
    }
    std::vector<ChestEspObservation> chests;
    if (showChests && currentCameraAvailable) {
        chests = snapshotClientKnownChests(cameraLevel, overlayCamera);
        if (!chests.empty() && !chestPresentationLogged.exchange(
                                       true, std::memory_order_acq_rel)) {
            char message[128]{};
            std::snprintf(
                    message, sizeof(message),
                    "chest ESP presentation: %zu client-known chest(s)",
                    chests.size());
            logLine(message);
        } else if (chests.empty() && clientKnownChestCount() != 0 &&
                   !chestLevelMismatchLogged.exchange(
                           true, std::memory_order_acq_rel)) {
            char message[192]{};
            std::snprintf(
                    message, sizeof(message),
                    "ERROR: chest ESP level mismatch: registry=%zu "
                    "camera_level=%zu",
                    clientKnownChestCount(),
                    clientKnownChestCountForLevel(cameraLevel));
            logLine(message);
        }
    }
    thread_local std::vector<OreEspObservation> cachedOres;
    thread_local const void* cachedOreLevel = nullptr;
    thread_local Vec3f cachedOreCameraPosition{};
    thread_local std::uint64_t cachedOreFrame = 0;
    std::span<const OreEspObservation> ores;
    if (showOres && currentCameraAvailable) {
        processClientOreRescan(cameraLevel, overlayCamera.position);
        const Vec3f movement = subtract(
                overlayCamera.position, cachedOreCameraPosition);
        const float movementSquared = movement.x * movement.x +
                movement.y * movement.y + movement.z * movement.z;
        constexpr std::uint64_t refreshIntervalFrames = 6;
        constexpr float immediateRefreshDistanceSquared = 64.0F;
        const bool refreshSnapshot = cachedOreLevel != cameraLevel ||
                currentFrame < cachedOreFrame ||
                currentFrame - cachedOreFrame >= refreshIntervalFrames ||
                movementSquared >= immediateRefreshDistanceSquared;
        if (refreshSnapshot) {
            cachedOres = snapshotClientKnownOres(
                    cameraLevel, overlayCamera, kMaximumOresPerFrame);
            cachedOreLevel = cameraLevel;
            cachedOreCameraPosition = overlayCamera.position;
            cachedOreFrame = currentFrame;
        }
        ores = cachedOres;
        if (!ores.empty() && !orePresentationLogged.exchange(
                                     true, std::memory_order_acq_rel)) {
            char message[128]{};
            std::snprintf(
                    message, sizeof(message),
                    "ore ESP presentation: %zu client-known ore(s)",
                    ores.size());
            logLine(message);
        } else if (ores.empty() && clientKnownOreCount() != 0 &&
                   clientKnownOreCountForLevel(cameraLevel) == 0 &&
                   !oreLevelMismatchLogged.exchange(
                           true, std::memory_order_acq_rel)) {
            char message[192]{};
            std::snprintf(
                    message, sizeof(message),
                    "ERROR: ore ESP level mismatch: registry=%zu "
                    "active_level=%zu",
                    clientKnownOreCount(),
                    clientKnownOreCountForLevel(cameraLevel));
            logLine(message);
        }
    }
    if (!overlayInstalled.load(std::memory_order_acquire)) {
        return;
    }
    if (program == 0 && !createRenderer()) {
        if (!rendererFailureLogged.exchange(true, std::memory_order_acq_rel))
            logLine("ERROR: entity overlay shader setup failed");
        return;
    }

    GLint viewport[4]{};
    gl.getIntegerv(GL_VIEWPORT, viewport);
    int surfaceWidth = 0;
    int surfaceHeight = 0;
    const bool hasSurfaceSize = launcherSurfaceSize(
            display, surface, surfaceWidth, surfaceHeight);
    const float width = static_cast<float>(
            hasSurfaceSize ? surfaceWidth : viewport[2]);
    const float height = static_cast<float>(
            hasSurfaceSize ? surfaceHeight : viewport[3]);
    if (width <= 0.0F || height <= 0.0F)
        return;
    const NetworkMetricsGeometry metricsGeometry =
            buildNetworkMetricsGeometry(metrics, width, height, performance);
    const PacketTrafficGeometry packetTrafficGeometry = packetTrafficEnabled
            ? buildPacketTrafficGeometry(packetTraffic, width, height)
            : PacketTrafficGeometry{};
    if (boxes.empty() && chests.empty() && ores.empty() &&
        metricsGeometry.shadowVertices.empty() &&
        packetTrafficGeometry.shadowVertices.empty())
        return;

    if (boxes.size() > largestBatchLogged && batchSamplesLogged < 8) {
        largestBatchLogged = boxes.size();
        ++batchSamplesLogged;
        const CapturedBox* largest = nullptr;
        float largestExtent = 0.0F;
        for (const auto& box : boxes) {
            const Vec3f extent = subtract(box.bounds.maximum, box.bounds.minimum);
            const float candidate = std::max({extent.x, extent.y, extent.z});
            if (candidate > largestExtent) {
                largestExtent = candidate;
                largest = &box;
            }
        }
        char message[384]{};
        if (largest != nullptr) {
            const Vec3f extent = subtract(largest->bounds.maximum, largest->bounds.minimum);
            std::snprintf(
                    message, sizeof(message),
                    "entity frame sample: actors=%zu largest=(%.3f,%.3f,%.3f) "
                    "bounds=(%.3f,%.3f,%.3f)->(%.3f,%.3f,%.3f) origin=(%.3f,%.3f,%.3f)",
                    boxes.size(), extent.x, extent.y, extent.z,
                    largest->bounds.minimum.x, largest->bounds.minimum.y,
                    largest->bounds.minimum.z, largest->bounds.maximum.x,
                    largest->bounds.maximum.y, largest->bounds.maximum.z,
                    overlayCamera.position.x, overlayCamera.position.y,
                    overlayCamera.position.z);
        } else {
            std::snprintf(message, sizeof(message),
                          "entity frame sample: actors=%zu", boxes.size());
        }
        logLine(message);
    }

    constexpr std::array<std::array<std::size_t, 2>, 12> edges{{
            {{0, 1}}, {{1, 2}}, {{2, 3}}, {{3, 0}},
            {{4, 5}}, {{5, 6}}, {{6, 7}}, {{7, 4}},
            {{0, 4}}, {{1, 5}}, {{2, 6}}, {{3, 7}},
    }};
    thread_local std::vector<float> vertices;
    vertices.clear();
    vertices.reserve(boxes.size() * edges.size() * 4);
    thread_local std::vector<float> chestVertices;
    chestVertices.clear();
    chestVertices.reserve(chests.size() * edges.size() * 4);
    constexpr std::size_t oreKindCount =
            static_cast<std::size_t>(OreKind::count);
    thread_local std::array<std::vector<float>, oreKindCount> oreVertices;
    for (auto& kindVertices : oreVertices) {
        kindVertices.clear();
        kindVertices.reserve(ores.size() * edges.size() * 4 / oreKindCount);
    }
    thread_local std::vector<float> locatorVertices;
    locatorVertices.clear();
    constexpr std::size_t compactCircleSegments = 12;
    locatorVertices.reserve(
            (boxes.size() + chests.size() + ores.size()) *
            compactCircleSegments * 4);
    const auto appendScreenLine = [&](float x1, float y1, float x2, float y2) {
        locatorVertices.push_back(x1 / width * 2.0F - 1.0F);
        locatorVertices.push_back(1.0F - y1 / height * 2.0F);
        locatorVertices.push_back(x2 / width * 2.0F - 1.0F);
        locatorVertices.push_back(1.0F - y2 / height * 2.0F);
    };
    const auto appendCompactCircle = [&](float centerX, float centerY) {
        constexpr float radius = 4.0F;
        constexpr float twoPi = 6.2831853071795864769F;
        for (std::size_t segment = 0; segment < compactCircleSegments; ++segment) {
            const float firstAngle = twoPi * static_cast<float>(segment) /
                    static_cast<float>(compactCircleSegments);
            const float secondAngle = twoPi * static_cast<float>(segment + 1) /
                    static_cast<float>(compactCircleSegments);
            appendScreenLine(
                    centerX + std::cos(firstAngle) * radius,
                    centerY + std::sin(firstAngle) * radius,
                    centerX + std::cos(secondAngle) * radius,
                    centerY + std::sin(secondAngle) * radius);
        }
    };
    std::size_t projectedCorners = 0;
    std::size_t locatorCount = 0;
    std::size_t nearPlaneClippedEdges = 0;
    float minimumScreenX = std::numeric_limits<float>::infinity();
    float maximumScreenX = -std::numeric_limits<float>::infinity();
    float minimumScreenY = std::numeric_limits<float>::infinity();
    float maximumScreenY = -std::numeric_limits<float>::infinity();
    const auto appendProjectedBox = [&](
            const EntityAabb& bounds, std::vector<float>& target,
            bool collectEntitySample) {
        const auto worldCorners = corners(bounds);
        std::array<std::array<float, 4>, 8> clipCorners{};
        std::array<ScreenPoint, 8> screenCorners{};
        float boxMinimumX = std::numeric_limits<float>::infinity();
        float boxMaximumX = -std::numeric_limits<float>::infinity();
        float boxMinimumY = std::numeric_limits<float>::infinity();
        float boxMaximumY = -std::numeric_limits<float>::infinity();
        for (std::size_t index = 0; index < worldCorners.size(); ++index) {
            clipCorners[index] = worldToClip(overlayCamera, worldCorners[index]);
            if (clipToScreen(
                        clipCorners[index], width, height,
                        screenCorners[index])) {
                if (collectEntitySample) {
                    ++projectedCorners;
                    minimumScreenX = std::min(
                            minimumScreenX, screenCorners[index].x);
                    maximumScreenX = std::max(
                            maximumScreenX, screenCorners[index].x);
                    minimumScreenY = std::min(
                            minimumScreenY, screenCorners[index].y);
                    maximumScreenY = std::max(
                            maximumScreenY, screenCorners[index].y);
                }
                boxMinimumX = std::min(boxMinimumX, screenCorners[index].x);
                boxMaximumX = std::max(boxMaximumX, screenCorners[index].x);
                boxMinimumY = std::min(boxMinimumY, screenCorners[index].y);
                boxMaximumY = std::max(boxMaximumY, screenCorners[index].y);
            }
        }
        if (std::isfinite(boxMinimumX) && shouldUseCompactEspMarker(
                boxMaximumX - boxMinimumX, boxMaximumY - boxMinimumY)) {
            const float centerX = (boxMinimumX + boxMaximumX) * 0.5F;
            const float centerY = (boxMinimumY + boxMaximumY) * 0.5F;
            appendCompactCircle(centerX, centerY);
            ++locatorCount;
            return;
        }
        for (const auto& edge : edges) {
            ScreenPoint first{};
            ScreenPoint second{};
            bool nearPlaneClipped = false;
            if (!projectClipSegment(
                        clipCorners[edge[0]], clipCorners[edge[1]], width,
                        height, first, second,
                        nearPlaneClipped)) {
                continue;
            }
            nearPlaneClippedEdges += nearPlaneClipped ? 1U : 0U;
            target.push_back(first.x / width * 2.0F - 1.0F);
            target.push_back(1.0F - first.y / height * 2.0F);
            target.push_back(second.x / width * 2.0F - 1.0F);
            target.push_back(1.0F - second.y / height * 2.0F);
        }
    };
    for (const auto& box : boxes) {
        appendProjectedBox(box.bounds, vertices, true);
    }
    for (const auto& chest : chests) {
        appendProjectedBox(chest.bounds, chestVertices, false);
    }
    for (const auto& ore : ores) {
        const auto kind = static_cast<std::size_t>(ore.kind);
        if (kind < oreVertices.size())
            appendProjectedBox(ore.bounds, oreVertices[kind], false);
    }
    if (nearPlaneClippedEdges != 0 &&
        !nearPlaneClipLogged.exchange(true, std::memory_order_acq_rel)) {
        char message[128]{};
        std::snprintf(
                message, sizeof(message),
                "entity overlay: stabilized %zu near-plane edge(s)",
                nearPlaneClippedEdges);
        logLine(message);
    }
    if (!boxes.empty() &&
        !synchronizedCameraLogged.exchange(true, std::memory_order_acq_rel)) {
        logLine("entity overlay: frame-synchronized camera active");
    }
    auto& projectionFlag = vertices.empty()
            ? failedProjectionLogged : successfulProjectionLogged;
    if (!boxes.empty() && !projectionFlag.exchange(true, std::memory_order_acq_rel)) {
        const auto& sample = boxes.front();
        const Vec3f center{
                (sample.bounds.minimum.x + sample.bounds.maximum.x) * 0.5F,
                (sample.bounds.minimum.y + sample.bounds.maximum.y) * 0.5F,
                (sample.bounds.minimum.z + sample.bounds.maximum.z) * 0.5F,
        };
        const Vec3f relativeCenter = subtract(center, overlayCamera.position);
        const auto centerEye = transform(
                overlayCamera.view,
                {{relativeCenter.x, relativeCenter.y, relativeCenter.z, 1.0F}});
        const auto centerClip = transform(overlayCamera.projection, centerEye);
        const float centerDepth = centerClip[3];
        char message[448]{};
        std::snprintf(
                message, sizeof(message),
                "entity projection sample: surface=%.0fx%.0f glViewport=%dx%d boxes=%zu depth=%.3f "
                "corners=%zu screen=(%.1f..%.1f,%.1f..%.1f) vertices=%zu locators=%zu",
                width, height, viewport[2], viewport[3], boxes.size(), centerDepth, projectedCorners,
                minimumScreenX, maximumScreenX, minimumScreenY, maximumScreenY,
                vertices.size() / 2, locatorCount);
        logLine(message);
    }

    GLint oldProgram{};
    GLint oldBuffer{};
    GLint oldFramebuffer{};
    GLint oldPositionAttributeEnabled{};
    GLint oldBlendSourceRgb{};
    GLint oldBlendDestinationRgb{};
    GLint oldBlendSourceAlpha{};
    GLint oldBlendDestinationAlpha{};
    GLfloat oldLineWidth{1.0F};
    gl.getIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    gl.getIntegerv(GL_ARRAY_BUFFER_BINDING, &oldBuffer);
    gl.getIntegerv(GL_FRAMEBUFFER_BINDING, &oldFramebuffer);
    gl.getVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &oldPositionAttributeEnabled);
    gl.getIntegerv(GL_BLEND_SRC_RGB, &oldBlendSourceRgb);
    gl.getIntegerv(GL_BLEND_DST_RGB, &oldBlendDestinationRgb);
    gl.getIntegerv(GL_BLEND_SRC_ALPHA, &oldBlendSourceAlpha);
    gl.getIntegerv(GL_BLEND_DST_ALPHA, &oldBlendDestinationAlpha);
    gl.getFloatv(GL_LINE_WIDTH, &oldLineWidth);
    const bool depthEnabled = gl.isEnabled(GL_DEPTH_TEST) == GL_TRUE;
    const bool blendEnabled = gl.isEnabled(GL_BLEND) == GL_TRUE;
    const bool cullEnabled = gl.isEnabled(GL_CULL_FACE) == GL_TRUE;
    const bool scissorEnabled = gl.isEnabled(GL_SCISSOR_TEST) == GL_TRUE;

    clearGlErrors();
    gl.bindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.viewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    const GLenum framebufferStatus = gl.checkFramebufferStatus(GL_FRAMEBUFFER);
    gl.disable(GL_DEPTH_TEST);
    gl.disable(GL_CULL_FACE);
    gl.disable(GL_SCISSOR_TEST);
    gl.enable(GL_BLEND);
    gl.blendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl.useProgram(program);
    gl.bindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    gl.bufferData(
            GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
            vertices.data(), GL_STREAM_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    if (!vertices.empty()) {
        drawLines(vertices.data(), vertices.size(), 0.0F, 0.0F, 0.0F, 0.85F, 2.0F);
        drawLines(vertices.data(), vertices.size(), 0.25F, 1.0F, 0.25F, 1.0F, 1.0F);
    }
    if (!chestVertices.empty()) {
        drawLines(chestVertices.data(), chestVertices.size(),
                  0.0F, 0.0F, 0.0F, 0.9F, 2.0F);
        drawLines(chestVertices.data(), chestVertices.size(),
                  1.0F, 0.65F, 0.05F, 1.0F, 1.0F);
    }
    constexpr std::array<std::array<float, 3>, oreKindCount> oreColors{{
            {{0.35F, 0.35F, 0.35F}},
            {{0.85F, 0.72F, 0.55F}},
            {{0.95F, 0.45F, 0.15F}},
            {{1.0F, 0.82F, 0.05F}},
            {{1.0F, 0.12F, 0.12F}},
            {{0.15F, 0.35F, 1.0F}},
            {{0.1F, 0.95F, 1.0F}},
            {{0.1F, 1.0F, 0.35F}},
            {{0.95F, 0.95F, 0.9F}},
            {{0.55F, 0.28F, 0.2F}},
    }};
    for (std::size_t kind = 0; kind < oreVertices.size(); ++kind) {
        const auto& kindVertices = oreVertices[kind];
        if (kindVertices.empty())
            continue;
        drawLines(
                kindVertices.data(), kindVertices.size(),
                0.0F, 0.0F, 0.0F, 0.9F, 2.0F);
        drawLines(
                kindVertices.data(), kindVertices.size(),
                oreColors[kind][0], oreColors[kind][1], oreColors[kind][2],
                1.0F, 1.0F);
    }
    if (!locatorVertices.empty()) {
        drawLines(locatorVertices.data(), locatorVertices.size(), 0.0F, 0.0F, 0.0F, 0.9F, 2.0F);
        drawLines(locatorVertices.data(), locatorVertices.size(), 1.0F, 0.2F, 0.65F, 1.0F, 1.0F);
    }
    if (!metricsGeometry.shadowVertices.empty()) {
        drawLines(metricsGeometry.shadowVertices.data(),
                  metricsGeometry.shadowVertices.size(),
                  0.0F, 0.0F, 0.0F, 0.9F,
                  metricsGeometry.lineWidth + 2.0F);
        drawLines(metricsGeometry.pingVertices.data(),
                  metricsGeometry.pingVertices.size(),
                  1.0F, 1.0F, 1.0F, 1.0F,
                  metricsGeometry.lineWidth);
        drawLines(metricsGeometry.tpsVertices.data(),
                  metricsGeometry.tpsVertices.size(),
                  0.35F, 1.0F, 0.45F, 1.0F,
                  metricsGeometry.lineWidth);
        drawLines(metricsGeometry.chunkVertices.data(),
                  metricsGeometry.chunkVertices.size(),
                  0.45F, 0.85F, 1.0F, 1.0F,
                  metricsGeometry.lineWidth);
        drawLines(metricsGeometry.pendingVertices.data(),
                  metricsGeometry.pendingVertices.size(),
                  1.0F, 0.8F, 0.3F, 1.0F,
                  metricsGeometry.lineWidth);
        drawLines(metricsGeometry.fpsVertices.data(),
                  metricsGeometry.fpsVertices.size(),
                  1.0F, 1.0F, 1.0F, 1.0F,
                  metricsGeometry.lineWidth);
        drawLines(metricsGeometry.memoryVertices.data(),
                  metricsGeometry.memoryVertices.size(),
                  0.8F, 0.65F, 1.0F, 1.0F,
                  metricsGeometry.lineWidth);
    }
    if (!packetTrafficGeometry.shadowVertices.empty()) {
        drawLines(packetTrafficGeometry.shadowVertices.data(),
                  packetTrafficGeometry.shadowVertices.size(),
                  0.0F, 0.0F, 0.0F, 0.92F,
                  packetTrafficGeometry.lineWidth + 2.0F);
        drawLines(packetTrafficGeometry.incomingVertices.data(),
                  packetTrafficGeometry.incomingVertices.size(),
                  0.35F, 0.9F, 1.0F, 1.0F,
                  packetTrafficGeometry.lineWidth);
        drawLines(packetTrafficGeometry.outgoingVertices.data(),
                  packetTrafficGeometry.outgoingVertices.size(),
                  1.0F, 0.72F, 0.3F, 1.0F,
                  packetTrafficGeometry.lineWidth);
    }
    const GLenum drawError = gl.getError();

    if (!presentationSampleLogged.exchange(true, std::memory_order_acq_rel)) {
        const auto* renderer = reinterpret_cast<const char*>(gl.getString(GL_RENDERER));
        char message[512]{};
        std::snprintf(
                message, sizeof(message),
                "entity presentation sample: surface=%.0fx%.0f viewport=%dx%d "
                "oldFramebuffer=%d defaultStatus=0x%x glError=0x%x renderer=%s",
                width, height, viewport[2], viewport[3], oldFramebuffer,
                static_cast<unsigned int>(framebufferStatus),
                static_cast<unsigned int>(drawError),
                renderer == nullptr ? "unknown" : renderer);
        logLine(message);
    }

    gl.lineWidth(oldLineWidth);
    gl.blendFuncSeparate(
            static_cast<GLenum>(oldBlendSourceRgb), static_cast<GLenum>(oldBlendDestinationRgb),
            static_cast<GLenum>(oldBlendSourceAlpha), static_cast<GLenum>(oldBlendDestinationAlpha));
    restoreCapability(GL_DEPTH_TEST, depthEnabled);
    restoreCapability(GL_BLEND, blendEnabled);
    restoreCapability(GL_CULL_FACE, cullEnabled);
    restoreCapability(GL_SCISSOR_TEST, scissorEnabled);
    if (oldPositionAttributeEnabled == GL_FALSE)
        gl.disableVertexAttribArray(0);
    gl.bindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(oldBuffer));
    gl.useProgram(static_cast<GLuint>(oldProgram));
    gl.bindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(oldFramebuffer));
    gl.viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
}

} // namespace

bool installEntityHitboxOverlay() {
    if (overlayInstalled.load(std::memory_order_acquire))
        return true;
    if (!resolveGlApi()) {
        logLine("ERROR: developer overlay unavailable; launcher GLES2 API missing");
        return false;
    }
    if (!addLauncherSwapBuffersCallback(nullptr, drawEntityHitboxes)) {
        logLine("ERROR: developer overlay unavailable; launcher render callback missing");
        return false;
    }
    overlayInstalled.store(true, std::memory_order_release);
    logLine("developer overlay: through-wall GLES2 renderer ready");
    return true;
}

} // namespace dobby

#else

namespace dobby {

bool installEntityHitboxOverlay() { return false; }

} // namespace dobby

#endif
