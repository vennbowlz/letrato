// audio.h - immediate audio feedback, synthesised at runtime (no sound files).
#pragma once
#include <SDL.h>

class Audio {
public:
    // Great / Good / TryAgain open with a camera-shutter sound: they play on SNAP.
    enum class Cue { Click, Great, Good, TryAgain };

    bool open();
    void close();
    void play(Cue cue);

private:
    void shutter(float* buf, int len);
    void tone(float* buf, int len, int offset, float freq, float seconds, float volume);
    SDL_AudioDeviceID dev_ = 0;
    int rate_ = 44100;
};
