#pragma once

enum class VideoResolution {
    P540,
    P720,
    P1080
};

enum class VideoFrameRate {
    Fps15,
    Fps30,
    Fps60
};

struct VideoQualitySettings {
    VideoResolution resolution = VideoResolution::P1080;
    VideoFrameRate frameRate = VideoFrameRate::Fps30;
};

inline bool operator==(const VideoQualitySettings &a, const VideoQualitySettings &b) {
    return a.resolution == b.resolution
        && a.frameRate == b.frameRate;
}

inline bool operator!=(const VideoQualitySettings &a, const VideoQualitySettings &b) {
    return !(a == b);
}

inline int videoQualityWidth(VideoResolution resolution) {
    switch (resolution) {
    case VideoResolution::P540:  return 960;
    case VideoResolution::P720:  return 1280;
    case VideoResolution::P1080: return 1920;
    }
    return 1920;
}

inline int videoQualityHeight(VideoResolution resolution) {
    switch (resolution) {
    case VideoResolution::P540:  return 540;
    case VideoResolution::P720:  return 720;
    case VideoResolution::P1080: return 1080;
    }
    return 1080;
}

inline int videoQualityMaxFPS(VideoFrameRate frameRate) {
    switch (frameRate) {
    case VideoFrameRate::Fps15: return 15;
    case VideoFrameRate::Fps30: return 30;
    case VideoFrameRate::Fps60: return 60;
    }
    return 30;
}

inline int videoQualityRefreshRate(VideoFrameRate /*frameRate*/) {
    return 60;
}

inline bool videoQualityH265Support() {
    return true;
}
