#ifndef SPECTROGRAM_H
#define SPECTROGRAM_H

#include "imgui/backends/imgui_impl_sdl.h"
#include "imgui_impl_opengl3.h"
#include "imgui/imgui.h"

#include <stdio.h>
#include <SDL.h>
#include <SDL_audio.h>
#include <SDL_mixer.h>
#include <GL/glew.h>
#include <FLAC/all.h>

typedef enum {
    MONO_CHANNEL = 0,
    LEFT_CHANNEL = 0,
    RIGHT_CHANNEL = 1,
    // We're not fiddling with more audio channels just yet
    MAX_CHANNELS // 2
} channel_idx_t;

typedef struct {
    int channels;
    int sample_rate;
    int bits_per_sample;
    int total_samples;
    int play_position;
    int write_position;
    int flac_read_callbacks;
    int flac_write_callbacks;
    int flac_error_callbacks;
    int sdl_mix_callbacks;
    // FLAC supports up to 8 audio channels
    unsigned char *buffers[8];

} flac_client_data_t;

#define MAX_INT16 32767


Mix_Music *load_sound_from_file(const char *filename);
SDL_AudioDeviceID sdl_custom_audio_init(const char *device);
void restart_song(void);
void rewind_song(float percent);
#endif
