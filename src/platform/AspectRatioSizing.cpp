#include "platform/AspectRatioSizing.h"

#include <algorithm>
#include <cmath>

namespace {
struct OuterSize {
    int width = 1;
    int height = 1;
};

int rectWidth(const RECT &rect) {
    return static_cast<int>(rect.right - rect.left);
}

int rectHeight(const RECT &rect) {
    return static_cast<int>(rect.bottom - rect.top);
}

int horizontalFrame(const AspectRatioFrameMargins &margins) {
    return std::max(0, margins.left) + std::max(0, margins.right);
}

int verticalFrame(const AspectRatioFrameMargins &margins) {
    return std::max(0, margins.top) + std::max(0, margins.bottom);
}

int clientWidthForOuterWidth(int outerWidth, const AspectRatioFrameMargins &margins) {
    return std::max(1, outerWidth - horizontalFrame(margins));
}

int clientHeightForOuterHeight(int outerHeight, const AspectRatioFrameMargins &margins) {
    return std::max(1, outerHeight - verticalFrame(margins));
}

int outerWidthForClientWidth(int clientWidth, const AspectRatioFrameMargins &margins) {
    return std::max(1, clientWidth) + horizontalFrame(margins);
}

int outerHeightForClientHeight(int clientHeight, const AspectRatioFrameMargins &margins) {
    return std::max(1, clientHeight) + verticalFrame(margins);
}

int rounded(double value) {
    return static_cast<int>(std::lround(value));
}

AspectRatioSizeConstraints normalized(AspectRatioSizeConstraints constraints) {
    constraints.minOuterWidth = std::max(1, constraints.minOuterWidth);
    constraints.minOuterHeight = std::max(1, constraints.minOuterHeight);
    constraints.maxOuterWidth = std::max(constraints.minOuterWidth, constraints.maxOuterWidth);
    constraints.maxOuterHeight = std::max(constraints.minOuterHeight, constraints.maxOuterHeight);
    return constraints;
}

void setHeightAroundCenter(RECT &rect, int outerHeight) {
    const int center = static_cast<int>((rect.top + rect.bottom) / 2);
    rect.top = center - outerHeight / 2;
    rect.bottom = rect.top + outerHeight;
}

void setWidthAroundCenter(RECT &rect, int outerWidth) {
    const int center = static_cast<int>((rect.left + rect.right) / 2);
    rect.left = center - outerWidth / 2;
    rect.right = rect.left + outerWidth;
}

OuterSize sizeDrivenByOuterWidth(int outerWidth, double targetRatio, const AspectRatioFrameMargins &margins) {
    const int clientWidth = clientWidthForOuterWidth(outerWidth, margins);
    return OuterSize{outerWidth, outerHeightForClientHeight(rounded(clientWidth / targetRatio), margins)};
}

OuterSize sizeDrivenByOuterHeight(int outerHeight, double targetRatio, const AspectRatioFrameMargins &margins) {
    const int clientHeight = clientHeightForOuterHeight(outerHeight, margins);
    return OuterSize{outerWidthForClientWidth(rounded(clientHeight * targetRatio), margins), outerHeight};
}

OuterSize constrainedSizeDrivenByWidth(
    int outerWidth,
    double targetRatio,
    const AspectRatioFrameMargins &margins,
    AspectRatioSizeConstraints constraints) {
    OuterSize size = sizeDrivenByOuterWidth(outerWidth, targetRatio, margins);
    for (int i = 0; i < 4; ++i) {
        if (size.width < constraints.minOuterWidth) {
            size = sizeDrivenByOuterWidth(constraints.minOuterWidth, targetRatio, margins);
        } else if (size.width > constraints.maxOuterWidth) {
            size = sizeDrivenByOuterWidth(constraints.maxOuterWidth, targetRatio, margins);
        } else if (size.height < constraints.minOuterHeight) {
            size = sizeDrivenByOuterHeight(constraints.minOuterHeight, targetRatio, margins);
        } else if (size.height > constraints.maxOuterHeight) {
            size = sizeDrivenByOuterHeight(constraints.maxOuterHeight, targetRatio, margins);
        } else {
            return size;
        }
    }
    size.width = std::clamp(size.width, constraints.minOuterWidth, constraints.maxOuterWidth);
    size.height = std::clamp(size.height, constraints.minOuterHeight, constraints.maxOuterHeight);
    return size;
}

OuterSize constrainedSizeDrivenByHeight(
    int outerHeight,
    double targetRatio,
    const AspectRatioFrameMargins &margins,
    AspectRatioSizeConstraints constraints) {
    OuterSize size = sizeDrivenByOuterHeight(outerHeight, targetRatio, margins);
    for (int i = 0; i < 4; ++i) {
        if (size.height < constraints.minOuterHeight) {
            size = sizeDrivenByOuterHeight(constraints.minOuterHeight, targetRatio, margins);
        } else if (size.height > constraints.maxOuterHeight) {
            size = sizeDrivenByOuterHeight(constraints.maxOuterHeight, targetRatio, margins);
        } else if (size.width < constraints.minOuterWidth) {
            size = sizeDrivenByOuterWidth(constraints.minOuterWidth, targetRatio, margins);
        } else if (size.width > constraints.maxOuterWidth) {
            size = sizeDrivenByOuterWidth(constraints.maxOuterWidth, targetRatio, margins);
        } else {
            return size;
        }
    }
    size.width = std::clamp(size.width, constraints.minOuterWidth, constraints.maxOuterWidth);
    size.height = std::clamp(size.height, constraints.minOuterHeight, constraints.maxOuterHeight);
    return size;
}

bool cornerShouldUseWidth(const RECT &rect, double targetRatio, const AspectRatioFrameMargins &margins) {
    const int clientWidth = clientWidthForOuterWidth(rectWidth(rect), margins);
    const int clientHeight = clientHeightForOuterHeight(rectHeight(rect), margins);
    return static_cast<double>(clientWidth) / clientHeight >= targetRatio;
}

void setWidthKeepingLeft(RECT &rect, int outerWidth) {
    rect.right = rect.left + outerWidth;
}

void setWidthKeepingRight(RECT &rect, int outerWidth) {
    rect.left = rect.right - outerWidth;
}

void setHeightKeepingTop(RECT &rect, int outerHeight) {
    rect.bottom = rect.top + outerHeight;
}

void setHeightKeepingBottom(RECT &rect, int outerHeight) {
    rect.top = rect.bottom - outerHeight;
}
}

bool adjustWindowRectForAspectRatio(
    RECT &rect,
    unsigned int sizingEdge,
    double targetRatio,
    AspectRatioFrameMargins margins,
    AspectRatioSizeConstraints constraints) {
    if (targetRatio <= 0.0 || rectWidth(rect) <= 0 || rectHeight(rect) <= 0) {
        return false;
    }
    constraints = normalized(constraints);

    switch (sizingEdge) {
    case WMSZ_LEFT: {
        const OuterSize size = constrainedSizeDrivenByWidth(rectWidth(rect), targetRatio, margins, constraints);
        setWidthKeepingRight(rect, size.width);
        setHeightAroundCenter(rect, size.height);
        return true;
    }
    case WMSZ_RIGHT: {
        const OuterSize size = constrainedSizeDrivenByWidth(rectWidth(rect), targetRatio, margins, constraints);
        setWidthKeepingLeft(rect, size.width);
        setHeightAroundCenter(rect, size.height);
        return true;
    }
    case WMSZ_TOP: {
        const OuterSize size = constrainedSizeDrivenByHeight(rectHeight(rect), targetRatio, margins, constraints);
        setWidthAroundCenter(rect, size.width);
        setHeightKeepingBottom(rect, size.height);
        return true;
    }
    case WMSZ_BOTTOM: {
        const OuterSize size = constrainedSizeDrivenByHeight(rectHeight(rect), targetRatio, margins, constraints);
        setWidthAroundCenter(rect, size.width);
        setHeightKeepingTop(rect, size.height);
        return true;
    }
    case WMSZ_TOPLEFT: {
        const OuterSize size = cornerShouldUseWidth(rect, targetRatio, margins)
            ? constrainedSizeDrivenByWidth(rectWidth(rect), targetRatio, margins, constraints)
            : constrainedSizeDrivenByHeight(rectHeight(rect), targetRatio, margins, constraints);
        setWidthKeepingRight(rect, size.width);
        setHeightKeepingBottom(rect, size.height);
        return true;
    }
    case WMSZ_TOPRIGHT: {
        const OuterSize size = cornerShouldUseWidth(rect, targetRatio, margins)
            ? constrainedSizeDrivenByWidth(rectWidth(rect), targetRatio, margins, constraints)
            : constrainedSizeDrivenByHeight(rectHeight(rect), targetRatio, margins, constraints);
        setWidthKeepingLeft(rect, size.width);
        setHeightKeepingBottom(rect, size.height);
        return true;
    }
    case WMSZ_BOTTOMLEFT: {
        const OuterSize size = cornerShouldUseWidth(rect, targetRatio, margins)
            ? constrainedSizeDrivenByWidth(rectWidth(rect), targetRatio, margins, constraints)
            : constrainedSizeDrivenByHeight(rectHeight(rect), targetRatio, margins, constraints);
        setWidthKeepingRight(rect, size.width);
        setHeightKeepingTop(rect, size.height);
        return true;
    }
    case WMSZ_BOTTOMRIGHT: {
        const OuterSize size = cornerShouldUseWidth(rect, targetRatio, margins)
            ? constrainedSizeDrivenByWidth(rectWidth(rect), targetRatio, margins, constraints)
            : constrainedSizeDrivenByHeight(rectHeight(rect), targetRatio, margins, constraints);
        setWidthKeepingLeft(rect, size.width);
        setHeightKeepingTop(rect, size.height);
        return true;
    }
    default:
        return false;
    }
}
