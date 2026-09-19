#include "ui/network_metrics_overlay.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace dobby {
namespace {

using Glyph = std::array<unsigned char, 7>;

Glyph glyph(char character) {
    switch (character) {
    case '0': return {{14, 17, 19, 21, 25, 17, 14}};
    case '1': return {{4, 12, 4, 4, 4, 4, 14}};
    case '2': return {{14, 17, 1, 2, 4, 8, 31}};
    case '3': return {{30, 1, 1, 14, 1, 1, 30}};
    case '4': return {{2, 6, 10, 18, 31, 2, 2}};
    case '5': return {{31, 16, 16, 30, 1, 1, 30}};
    case '6': return {{14, 16, 16, 30, 17, 17, 14}};
    case '7': return {{31, 1, 2, 4, 8, 8, 8}};
    case '8': return {{14, 17, 17, 14, 17, 17, 14}};
    case '9': return {{14, 17, 17, 15, 1, 1, 14}};
    case 'B': return {{30, 17, 17, 30, 17, 17, 30}};
    case 'C': return {{14, 17, 16, 16, 16, 17, 14}};
    case 'D': return {{30, 17, 17, 17, 17, 17, 30}};
    case 'E': return {{31, 16, 16, 30, 16, 16, 31}};
    case 'F': return {{31, 16, 16, 30, 16, 16, 16}};
    case 'G': return {{14, 17, 16, 23, 17, 17, 15}};
    case 'H': return {{17, 17, 17, 31, 17, 17, 17}};
    case 'I': return {{14, 4, 4, 4, 4, 4, 14}};
    case 'M': return {{17, 27, 21, 21, 17, 17, 17}};
    case 'N': return {{17, 25, 21, 19, 17, 17, 17}};
    case 'P': return {{30, 17, 17, 30, 16, 16, 16}};
    case 'R': return {{30, 17, 17, 30, 20, 18, 17}};
    case 'K': return {{17, 18, 20, 24, 20, 18, 17}};
    case 'S': return {{15, 16, 16, 14, 1, 1, 30}};
    case 'T': return {{31, 4, 4, 4, 4, 4, 4}};
    case 'U': return {{17, 17, 17, 17, 17, 17, 14}};
    case '(': return {{2, 4, 8, 8, 8, 4, 2}};
    case ')': return {{8, 4, 2, 2, 2, 4, 8}};
    case '/': return {{1, 2, 2, 4, 8, 8, 16}};
    case '.': return {{0, 0, 0, 0, 0, 6, 6}};
    case '-': return {{0, 0, 0, 31, 0, 0, 0}};
    case '~': return {{0, 0, 9, 22, 0, 0, 0}};
    default: return {};
    }
}

float textWidth(std::string_view text, float pixel) {
    if (text.empty())
        return 0.0F;
    return static_cast<float>(text.size() * 6U - 1U) * pixel;
}

void appendSegment(std::vector<float>& vertices, float x1, float y1,
                   float x2, float y2, float width, float height) {
    vertices.push_back(x1 / width * 2.0F - 1.0F);
    vertices.push_back(1.0F - y1 / height * 2.0F);
    vertices.push_back(x2 / width * 2.0F - 1.0F);
    vertices.push_back(1.0F - y2 / height * 2.0F);
}

void appendText(std::vector<float>& vertices, std::string_view text,
                float x, float y, float pixel, float width, float height) {
    for (const char character : text) {
        const Glyph rows = glyph(character);
        for (std::size_t row = 0; row < rows.size(); ++row) {
            for (std::size_t column = 0; column < 5; ++column) {
                if ((rows[row] & (1U << (4U - column))) == 0)
                    continue;
                const float left = x + static_cast<float>(column) * pixel;
                const float center = y + (static_cast<float>(row) + 0.5F) * pixel;
                appendSegment(vertices, left, center, left + pixel, center,
                              width, height);
            }
        }
        x += 6.0F * pixel;
    }
}

} // namespace

NetworkMetricsText formatNetworkMetrics(
        const NetworkMetricsSnapshot& metrics,
        const ClientPerformanceSnapshot& performance) {
    NetworkMetricsText result;
    if (metrics.connected) {
        result.ping = metrics.pingMilliseconds
                ? "PING " + std::to_string(*metrics.pingMilliseconds) + " MS"
                : "PING --";
        if (metrics.observedTicksPerSecond) {
            char value[32]{};
            std::snprintf(value, sizeof(value), "TPS~ %.1f",
                          *metrics.observedTicksPerSecond);
            result.observedTps = value;
        } else {
            result.observedTps = "TPS~ --";
        }
        result.chunks = "CHUNKS " + std::to_string(metrics.loadedChunks) +
                " (" + std::to_string(metrics.chunksPerSecond) + "/S)";
        result.pending = metrics.outstandingSubChunkRequests
                ? "PENDING " +
                        std::to_string(*metrics.outstandingSubChunkRequests)
                : "PENDING --";
    }
    if (performance.framesPerSecond) {
        char value[32]{};
        std::snprintf(value, sizeof(value), "FPS %.0f",
                      *performance.framesPerSecond);
        result.framesPerSecond = value;
    }
    if (performance.residentBytes) {
        constexpr double bytesPerMegabyte = 1024.0 * 1024.0;
        constexpr double bytesPerGigabyte = 1024.0 * bytesPerMegabyte;
        char value[32]{};
        if (static_cast<double>(*performance.residentBytes) >=
            bytesPerGigabyte) {
            std::snprintf(
                    value, sizeof(value), "MEM %.1f GB",
                    static_cast<double>(*performance.residentBytes) /
                            bytesPerGigabyte);
        } else {
            std::snprintf(
                    value, sizeof(value), "MEM %.0f MB",
                    static_cast<double>(*performance.residentBytes) /
                            bytesPerMegabyte);
        }
        result.residentMemory = value;
    }
    result.visible = !result.ping.empty() || !result.framesPerSecond.empty() ||
            !result.residentMemory.empty();
    return result;
}

NetworkMetricsGeometry buildNetworkMetricsGeometry(
        const NetworkMetricsSnapshot& metrics, float surfaceWidth,
        float surfaceHeight, const ClientPerformanceSnapshot& performance) {
    NetworkMetricsGeometry result;
    const auto text = formatNetworkMetrics(metrics, performance);
    if (!text.visible || !std::isfinite(surfaceWidth) ||
        !std::isfinite(surfaceHeight) || surfaceWidth <= 0.0F ||
        surfaceHeight <= 0.0F) {
        return result;
    }

    const float pixel = std::clamp(
            std::round(surfaceHeight / 360.0F), 2.0F, 4.0F);
    const float margin = 6.0F * pixel;
    const float lineHeight = 9.0F * pixel;
    const float longest = std::max({
            textWidth(text.ping, pixel), textWidth(text.observedTps, pixel),
            textWidth(text.chunks, pixel), textWidth(text.pending, pixel),
            textWidth(text.framesPerSecond, pixel),
            textWidth(text.residentMemory, pixel)});
    const float x = std::max(0.0F, surfaceWidth - margin - longest);
    float y = margin;
    const auto appendLine = [&](std::vector<float>& vertices,
                                const std::string& value) {
        if (value.empty())
            return;
        appendText(vertices, value, x, y, pixel, surfaceWidth, surfaceHeight);
        appendText(result.shadowVertices, value, x + pixel, y + pixel, pixel,
                   surfaceWidth, surfaceHeight);
        y += lineHeight;
    };
    appendLine(result.pingVertices, text.ping);
    appendLine(result.tpsVertices, text.observedTps);
    appendLine(result.chunkVertices, text.chunks);
    appendLine(result.pendingVertices, text.pending);
    appendLine(result.fpsVertices, text.framesPerSecond);
    appendLine(result.memoryVertices, text.residentMemory);
    result.lineWidth = pixel;
    return result;
}

} // namespace dobby
