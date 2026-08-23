// Captures X11 screen frames using XShm extension at 35 FPS

#ifndef CLOUD_GAMING_CAPTURE_H
#define CLOUD_GAMING_CAPTURE_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <thread>

class FrameCapture {
public:
    FrameCapture(const char* display_name = nullptr) {
        display = XOpenDisplay(display_name);
        if (!display) {
            throw std::runtime_error("Failed to open X display");
        }

        screen = DefaultScreen(display);
        root = RootWindow(display, screen);

        XWindowAttributes attributes;
        XGetWindowAttributes(display, root, &attributes);
        // YUV420 chroma subsampling needs even dimensions, crop by a pixel if the screen isn't
        width = attributes.width & ~1;
        height = attributes.height & ~1;

        if (!XShmQueryExtension(display)) {
            XCloseDisplay(display);
            throw std::runtime_error("XShm extension not available");
        }

        image = XShmCreateImage(display, DefaultVisual(display, screen),
                                attributes.depth, ZPixmap, NULL, &shminfo, width, height);
        if (!image) {
            XCloseDisplay(display);
            throw std::runtime_error("Failed to create XShm image");
        }

        shminfo.shmid = shmget(IPC_PRIVATE, image->bytes_per_line * image->height, IPC_CREAT | 0777);
        if (shminfo.shmid < 0) {
            XDestroyImage(image);
            XCloseDisplay(display);
            throw std::runtime_error("Failed to allocate shared memory segment");
        }

        shminfo.shmaddr = image->data = (char*)shmat(shminfo.shmid, 0, 0);
        shminfo.readOnly = False;

        if (!XShmAttach(display, &shminfo)) {
            shmdt(shminfo.shmaddr);
            shmctl(shminfo.shmid, IPC_RMID, 0);
            XDestroyImage(image);
            XCloseDisplay(display);
            throw std::runtime_error("Failed to attach shared memory");
        }

        XSync(display, False);

        // Preallocate buffer to copy data out of SHM segment
        pixel_data.resize(width * height * 4); 
    }

    ~FrameCapture() {
        if (display && image) {
            XShmDetach(display, &shminfo);
            XDestroyImage(image);
            shmdt(shminfo.shmaddr);
            shmctl(shminfo.shmid, IPC_RMID, 0);
            XCloseDisplay(display);
        }
    }

    int GetWidth() const { return width; }
    int GetHeight() const { return height; }

    // Blocks to maintain ~35fps, returns a pointer to the captured BGRA pixel data
    const uint8_t* CaptureFrame() {
        auto now = std::chrono::steady_clock::now();
        auto target_time = last_capture_time + std::chrono::microseconds(28571); // 1000000us / 35
        
        if (now < target_time) {
            std::this_thread::sleep_until(target_time);
        }
        
        XShmGetImage(display, root, image, 0, 0, AllPlanes);
        // Note: XShmGetImage writes directly into image->data which we mapped
        std::copy(image->data, image->data + (width * height * 4), pixel_data.begin());

        last_capture_time = std::chrono::steady_clock::now();
        return pixel_data.data();
    }

private:
    Display* display = nullptr;
    int screen = 0;
    Window root = 0;
    XImage* image = nullptr;
    XShmSegmentInfo shminfo;
    int width = 0;
    int height = 0;

    std::vector<uint8_t> pixel_data;
    std::chrono::steady_clock::time_point last_capture_time = std::chrono::steady_clock::now();
};

#endif // CLOUD_GAMING_CAPTURE_H