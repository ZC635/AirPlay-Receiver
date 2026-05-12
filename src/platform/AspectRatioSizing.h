#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <limits>

struct AspectRatioFrameMargins {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct AspectRatioSizeConstraints {
    int minOuterWidth = 1;
    int minOuterHeight = 1;
    int maxOuterWidth = std::numeric_limits<int>::max() / 4;
    int maxOuterHeight = std::numeric_limits<int>::max() / 4;
};

struct AspectRatioOuterSize {
    int width = 1;
    int height = 1;
};

int clientWidthForOuterWidth(int outerWidth, AspectRatioFrameMargins margins);
int clientHeightForOuterHeight(int outerHeight, AspectRatioFrameMargins margins);

AspectRatioOuterSize adjustedOuterSizeDrivenByHeight(
    int outerHeight,
    double targetRatio,
    AspectRatioFrameMargins margins,
    AspectRatioSizeConstraints constraints = {});

bool adjustWindowRectForAspectRatio(
    RECT &rect,
    unsigned int sizingEdge,
    double targetRatio,
    AspectRatioFrameMargins margins,
    AspectRatioSizeConstraints constraints = {});

void updateWindowPosCopyBitsForResize(WINDOWPOS &windowPos);
