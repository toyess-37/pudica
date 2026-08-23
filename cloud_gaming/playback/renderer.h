#pragma once
#include <SDL2/SDL.h>
#include <stdexcept>
#include <iostream>

class VideoRenderer {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    int width, height;

public:
    VideoRenderer(int w, int h) : width(w), height(h) {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            throw std::runtime_error("SDL_Init failed");
        }

        window = SDL_CreateWindow("Pudica Cloud Gaming Receiver",
                                  SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                  width, height, SDL_WINDOW_SHOWN);
        if (!window) throw std::runtime_error("SDL_CreateWindow failed");

        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) throw std::runtime_error("SDL_CreateRenderer failed");

        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, 
                                    SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!texture) throw std::runtime_error("SDL_CreateTexture failed");
    }

    ~VideoRenderer() {
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    void Render(const uint8_t* bgra_data, int pitch) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                throw std::runtime_error("window closed");
            }
        }
        
        SDL_UpdateTexture(texture, nullptr, bgra_data, pitch);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }
};
