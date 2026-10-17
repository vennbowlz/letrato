// audio.cpp - soft sine chimes queued through SDL's audio device.
// Volumes are deliberately low so feedback encourages without distracting.
#include "audio.h"

#include <cmath>
#include <vector>

bool Audio::open() {
    SDL_AudioSpec want{}, have{};
    want.freq = rate_;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = 1024;
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!dev_) return false;
    rate_ = have.freq;
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void Audio::close() {
    if (dev_) SDL_CloseAudioDevice(dev_);
    dev_ = 0;
}

void Audio::tone(float* buf, int len, int offset, float freq, float seconds, float volume) {
    int n = (int)(seconds * rate_);
    for (int i = 0; i < n && offset + i < len; ++i) {
        float t = (float)i / rate_;
        float attack = std::fmin(1.0f, t / 0.008f);        // no click at onset
        float decay = std::exp(-t * 6.0f / seconds);
        float s = std::sin(2.0f * 3.14159265f * freq * t) +
                  0.25f * std::sin(4.0f * 3.14159265f * freq * t);   // gentle bell overtone
        buf[offset + i] += s * attack * decay * volume;
    }
}

// Two short filtered-noise bursts: the "click-clack" of a shutter.
void Audio::shutter(float* buf, int len) {
    Uint32 seed = 0x2545F491u;
    float lp = 0;
    for (int burst = 0; burst < 2; ++burst) {
        int start = burst * rate_ / 14, n = rate_ / 30;
        for (int i = 0; i < n && start + i < len; ++i) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            float noise = (seed & 0xFFFF) / 32768.0f - 1.0f;
            lp += 0.35f * (noise - lp);                       // soften the hiss
            float env = std::exp(-(float)i / n * 7.0f);
            buf[start + i] += lp * env * (burst ? 0.16f : 0.22f);
        }
    }
}

void Audio::play(Cue cue) {
    if (!dev_) return;
    std::vector<float> buf((size_t)(rate_ * 1.5f), 0.0f);
    int len = (int)buf.size(), step = rate_ / 8;
    int snap = 0;   // result cues start after the shutter
    if (cue != Cue::Click) { shutter(buf.data(), len); snap = rate_ / 4; }
    const float C5 = 523.25f, E5 = 659.25f, G5 = 783.99f, C6 = 1046.5f, G4 = 392.0f, E4 = 329.63f;
    switch (cue) {
        case Cue::Click:    tone(buf.data(), len, 0, 1200.0f, 0.04f, 0.05f); len = rate_ / 20; break;
        case Cue::Great:    tone(buf.data(), len, snap, C5, 0.5f, 0.12f);
                            tone(buf.data(), len, snap + step, E5, 0.5f, 0.12f);
                            tone(buf.data(), len, snap + step * 2, G5, 0.5f, 0.12f);
                            tone(buf.data(), len, snap + step * 3, C6, 0.8f, 0.12f); break;
        case Cue::Good:     tone(buf.data(), len, snap, E5, 0.5f, 0.12f);
                            tone(buf.data(), len, snap + step, G5, 0.7f, 0.12f); break;
        case Cue::TryAgain: tone(buf.data(), len, snap, G4, 0.5f, 0.10f);
                            tone(buf.data(), len, snap + step * 2, E4, 0.7f, 0.10f); break;
    }
    SDL_ClearQueuedAudio(dev_);
    SDL_QueueAudio(dev_, buf.data(), (Uint32)(len * sizeof(float)));
}
