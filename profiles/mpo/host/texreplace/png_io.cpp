// SPDX-License-Identifier: GPL-3.0-or-later

#include "png_io.hpp"

#include <cstring>

#if MPO_TEXREPLACE_SDL
#include <SDL3/SDL.h>
#endif

namespace mpo::texreplace {

#if MPO_TEXREPLACE_SDL

bool HavePng() {
    return true;
}

bool LoadPng(const std::filesystem::path &path, int *width, int *height,
             std::vector<std::uint8_t> *rgba, std::string *error) {
    // SDL takes UTF-8 file names on every platform.
    const std::u8string name = path.u8string();
    SDL_Surface *loaded = SDL_LoadPNG(reinterpret_cast<const char *>(name.c_str()));
    if (loaded == nullptr) {
        *error = path.string() + ": " + SDL_GetError();
        return false;
    }
    SDL_Surface *surface = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (surface == nullptr) {
        *error = path.string() + ": " + SDL_GetError();
        return false;
    }
    *width = surface->w;
    *height = surface->h;
    const auto row = static_cast<std::size_t>(surface->w) * 4;
    rgba->resize(row * static_cast<std::size_t>(surface->h));
    const auto *src = static_cast<const std::uint8_t *>(surface->pixels);
    for (int y = 0; y < surface->h; ++y) {
        std::memcpy(rgba->data() + static_cast<std::size_t>(y) * row,
                    src + static_cast<std::size_t>(y) * static_cast<std::size_t>(surface->pitch),
                    row);
    }
    SDL_DestroySurface(surface);
    return true;
}

bool SavePng(const std::filesystem::path &path, int width, int height,
             const std::vector<std::uint8_t> &rgba, std::string *error) {
    if (width <= 0 || height <= 0 ||
        rgba.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) {
        *error = path.string() + ": bad image size";
        return false;
    }
    SDL_Surface *surface = SDL_CreateSurfaceFrom(
        width, height, SDL_PIXELFORMAT_RGBA32, const_cast<std::uint8_t *>(rgba.data()), width * 4);
    if (surface == nullptr) {
        *error = path.string() + ": " + SDL_GetError();
        return false;
    }
    const std::u8string name = path.u8string();
    const bool ok = SDL_SavePNG(surface, reinterpret_cast<const char *>(name.c_str()));
    if (!ok) *error = path.string() + ": " + SDL_GetError();
    SDL_DestroySurface(surface);
    return ok;
}

#else

bool HavePng() {
    return false;
}

bool LoadPng(const std::filesystem::path &path, int *, int *, std::vector<std::uint8_t> *,
             std::string *error) {
    *error = path.string() + ": this build has no PNG support (it needs SDL3)";
    return false;
}

bool SavePng(const std::filesystem::path &path, int, int, const std::vector<std::uint8_t> &,
             std::string *error) {
    *error = path.string() + ": this build has no PNG support (it needs SDL3)";
    return false;
}

#endif

} // namespace mpo::texreplace
