// SPDX-License-Identifier: GPL-3.0-or-later

#include "sdl_audio_output.h"

#include <SDL3/SDL.h>

namespace mpo::audio {

SdlAudioOutput::~SdlAudioOutput() {
    Close();
}

bool SdlAudioOutput::Open(StereoResampler *resampler, const Options &options, std::string *error) {
    Close();
    if (resampler == nullptr) {
        *error = "no resampler";
        return false;
    }
    if (!options.driver.empty()) {
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, options.driver.c_str());
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        *error = std::string("SDL_InitSubSystem(AUDIO): ") + SDL_GetError();
        return false;
    }
    initedSubsystem_ = true;

    SDL_AudioSpec deviceSpec{};
    int frames = 0;
    if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &deviceSpec, &frames)) {
        deviceSpec.freq = 48000;
        frames = 0;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    // Ask for the device rate so SDL does not resample a second time; the
    // StereoResampler already converts from 44.1 kHz.
    spec.freq = options.sample_rate > 0 ? options.sample_rate : deviceSpec.freq;
    if (spec.freq <= 0) {
        spec.freq = 48000;
    }

    resampler_ = resampler;
    sampleRate_ = spec.freq;
    deviceFrames_ = frames;
    framesPulled_ = 0;
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                        &SdlAudioOutput::Callback, this);
    if (stream_ == nullptr) {
        *error = std::string("SDL_OpenAudioDeviceStream: ") + SDL_GetError();
        Close();
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
        *error = std::string("SDL_ResumeAudioStreamDevice: ") + SDL_GetError();
        Close();
        return false;
    }
    return true;
}

void SdlAudioOutput::Close() {
    if (stream_ != nullptr) {
        // Destroying the stream stops the callback before returning.
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    if (initedSubsystem_) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        initedSubsystem_ = false;
    }
    resampler_ = nullptr;
}

std::string SdlAudioOutput::DriverName() const {
    const char *name = SDL_GetCurrentAudioDriver();
    return name ? name : "";
}

void SdlAudioOutput::Callback(void *userdata, SDL_AudioStream *stream, int additional_amount,
                              int /*total_amount*/) {
    auto *self = static_cast<SdlAudioOutput *>(userdata);
    if (additional_amount <= 0 || self->resampler_ == nullptr) {
        return;
    }
    constexpr int kBytesPerFrame = 2 * static_cast<int>(sizeof(int16_t));
    const int frames = (additional_amount + kBytesPerFrame - 1) / kBytesPerFrame;
    self->scratch_.resize(static_cast<size_t>(frames) * 2);
    self->resampler_->Mix(self->scratch_.data(), static_cast<unsigned int>(frames),
                          self->sampleRate_);
    SDL_PutAudioStreamData(stream, self->scratch_.data(), frames * kBytesPerFrame);
    self->framesPulled_ += static_cast<uint64_t>(frames);
}

} // namespace mpo::audio
