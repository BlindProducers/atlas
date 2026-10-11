#pragma once
#include <vector>
#include <array>
#include <mutex>

constexpr int NUM_LFO_PATTERNS = 8;
constexpr int MAX_LFO_POINTS = 256;

struct LFOPoint
{
    float x    = 0;    // 0-1 position within pattern
    float y    = 0;    // 0-1 value
    float skew = 0.5f; // 0-1 curve shape (0.5 = linear)
};

enum class LFOLoopType
{
    Loop,
    Retrig,
    OneShot,
    Count
};

enum class LFOShapePreset
{
    Flat,
    LinearAsc,
    LinearDesc,
    ConcaveAsc,
    ConcaveDesc,
    ConvexAsc,
    ConvexDesc,
    CosineAsc,
    CosineDesc,
    TriangleUp,
    TriangleDown,
    Count
};

inline const char* lfoShapePresetName (LFOShapePreset s)
{
    switch (s)
    {
        case LFOShapePreset::Flat:         return "Flat";
        case LFOShapePreset::LinearAsc:    return "Linear Up";
        case LFOShapePreset::LinearDesc:   return "Linear Down";
        case LFOShapePreset::ConcaveAsc:   return "Concave Up";
        case LFOShapePreset::ConcaveDesc:  return "Concave Down";
        case LFOShapePreset::ConvexAsc:    return "Convex Up";
        case LFOShapePreset::ConvexDesc:   return "Convex Down";
        case LFOShapePreset::CosineAsc:    return "Cosine Up";
        case LFOShapePreset::CosineDesc:   return "Cosine Down";
        case LFOShapePreset::TriangleUp:   return "Triangle Up";
        case LFOShapePreset::TriangleDown: return "Triangle Down";
        default: return "";
    }
}

// Generate preset shape points
std::vector<LFOPoint> generatePresetShape (LFOShapePreset preset);

struct LFOPatternData
{
    std::vector<LFOPoint> points;
    int gridX = 4;
    int gridY = 4;

    LFOPatternData()
    {
        // Default: linear ramp 0->1
        points.push_back ({ 0.0f, 0.0f, 0.5f });
        points.push_back ({ 0.5f, 1.0f, 0.5f });
    }
};

struct LFOData
{
    std::array<LFOPatternData, NUM_LFO_PATTERNS> patterns;
    double phase = 0;
    LFOLoopType loopType = LFOLoopType::Retrig;

    // Thread safety for point access
    mutable std::mutex mutex;

    // Get a copy of points for the audio thread
    std::vector<LFOPoint> getPoints (int patternIdx) const
    {
        std::lock_guard<std::mutex> lock (mutex);
        return patterns[patternIdx].points;
    }

    void setPoints (int patternIdx, const std::vector<LFOPoint>& pts)
    {
        std::lock_guard<std::mutex> lock (mutex);
        patterns[patternIdx].points = pts;
    }
};
