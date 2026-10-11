#include "LFOData.h"
#include <cmath>

std::vector<LFOPoint> generatePresetShape (LFOShapePreset preset)
{
    std::vector<LFOPoint> pts;

    switch (preset)
    {
        case LFOShapePreset::Flat:
            pts.push_back ({ 0.0f, 0.5f, 0.5f });
            break;

        case LFOShapePreset::LinearAsc:
            pts.push_back ({ 0.0f, 0.0f, 0.5f });
            pts.push_back ({ 0.5f, 1.0f, 0.5f });
            break;

        case LFOShapePreset::LinearDesc:
            pts.push_back ({ 0.0f, 1.0f, 0.5f });
            pts.push_back ({ 0.5f, 0.0f, 0.5f });
            break;

        case LFOShapePreset::ConcaveAsc:
            pts.push_back ({ 0.0f, 0.0f, 0.8f });
            pts.push_back ({ 0.5f, 1.0f, 0.5f });
            break;

        case LFOShapePreset::ConcaveDesc:
            pts.push_back ({ 0.0f, 1.0f, 0.2f });
            pts.push_back ({ 0.5f, 0.0f, 0.5f });
            break;

        case LFOShapePreset::ConvexAsc:
            pts.push_back ({ 0.0f, 0.0f, 0.2f });
            pts.push_back ({ 0.5f, 1.0f, 0.5f });
            break;

        case LFOShapePreset::ConvexDesc:
            pts.push_back ({ 0.0f, 1.0f, 0.8f });
            pts.push_back ({ 0.5f, 0.0f, 0.5f });
            break;

        case LFOShapePreset::CosineAsc:
            pts.push_back ({ 0.0f, 0.0f, 0.5f });
            pts.push_back ({ 0.25f, 0.5f, 0.5f });
            pts.push_back ({ 0.5f, 1.0f, 0.5f });
            break;

        case LFOShapePreset::CosineDesc:
            pts.push_back ({ 0.0f, 1.0f, 0.5f });
            pts.push_back ({ 0.25f, 0.5f, 0.5f });
            pts.push_back ({ 0.5f, 0.0f, 0.5f });
            break;

        case LFOShapePreset::TriangleUp:
            pts.push_back ({ 0.0f, 0.0f, 0.5f });
            pts.push_back ({ 0.25f, 1.0f, 0.5f });
            pts.push_back ({ 0.5f, 0.0f, 0.5f });
            break;

        case LFOShapePreset::TriangleDown:
            pts.push_back ({ 0.0f, 1.0f, 0.5f });
            pts.push_back ({ 0.25f, 0.0f, 0.5f });
            pts.push_back ({ 0.5f, 1.0f, 0.5f });
            break;

        default:
            pts.push_back ({ 0.0f, 0.0f, 0.5f });
            pts.push_back ({ 0.5f, 1.0f, 0.5f });
            break;
    }

    return pts;
}
