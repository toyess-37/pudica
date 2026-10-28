#pragma once
#include <SDL2/SDL.h>
#include <stdexcept>
#include <string>
class VideoRenderer {
    SDL_Window* window=nullptr;SDL_Renderer* renderer=nullptr;SDL_Texture* texture=nullptr;
    bool initialized=false;
    void release(){if(texture)SDL_DestroyTexture(texture);if(renderer)SDL_DestroyRenderer(renderer);if(window)SDL_DestroyWindow(window);if(initialized)SDL_Quit();}
    static void check(bool ok,const char* operation){if(!ok)throw std::runtime_error(std::string(operation)+": "+SDL_GetError());}
public:
    VideoRenderer(int w,int h) {
        try {
            check(SDL_Init(SDL_INIT_VIDEO)==0,"SDL init");initialized=true;
            window=SDL_CreateWindow("Pudica Receiver",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,w,h,SDL_WINDOW_SHOWN);
            check(window,"SDL window");
            renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED);
            if(!renderer)renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
            check(renderer,"SDL renderer");
            texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,w,h);
            check(texture,"SDL texture");
        } catch(...) {release();throw;}
    }
    ~VideoRenderer(){release();}
    VideoRenderer(const VideoRenderer&)=delete;
    VideoRenderer& operator=(const VideoRenderer&)=delete;
    bool PollEvents(){SDL_Event e;while(SDL_PollEvent(&e))if(e.type==SDL_QUIT)return false;return true;}
    void Render(const uint8_t* data,int pitch) {
        check(SDL_UpdateTexture(texture,nullptr,data,pitch)==0,"SDL texture update");
        check(SDL_RenderClear(renderer)==0,"SDL clear");
        check(SDL_RenderCopy(renderer,texture,nullptr,nullptr)==0,"SDL copy");SDL_RenderPresent(renderer);
    }
};
