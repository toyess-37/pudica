#pragma once
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

class FrameCapture {
    Display* display=nullptr;
    Window root=0;
    XImage* image=nullptr;
    XShmSegmentInfo shared{};
    bool attached=false;
    int width=0,height=0;
    std::vector<uint8_t> pixels;
    void release() {
        if(attached){XShmDetach(display,&shared);XSync(display,False);}
        if(image){image->data=nullptr;XDestroyImage(image);}
        if(shared.shmaddr && shared.shmaddr!=reinterpret_cast<char*>(-1)) shmdt(shared.shmaddr);
        if(shared.shmid>=0) shmctl(shared.shmid,IPC_RMID,nullptr);
        if(display) XCloseDisplay(display);
    }
public:
    explicit FrameCapture(const char* name=nullptr) {
        shared.shmid=-1;
        try {
            display=XOpenDisplay(name);
            if(!display) throw std::runtime_error("cannot open X display");
            int screen=DefaultScreen(display);root=RootWindow(display,screen);
            XWindowAttributes a{};
            if(!XGetWindowAttributes(display,root,&a)||!XShmQueryExtension(display))
                throw std::runtime_error("XShm screen capture unavailable");
            width=a.width&~1;height=a.height&~1;
            if(width<2||height<2||width>8192||height>8192) throw std::runtime_error("invalid screen dimensions");
            image=XShmCreateImage(display,DefaultVisual(display,screen),a.depth,ZPixmap,nullptr,&shared,width,height);
            if(!image) throw std::runtime_error("cannot create XShm image");
            if(image->bits_per_pixel!=32||image->byte_order!=LSBFirst||
               image->red_mask!=0xff0000||image->green_mask!=0xff00||image->blue_mask!=0xff)
                throw std::runtime_error("capture requires a 32-bit little-endian BGRA X visual");
            shared.shmid=shmget(IPC_PRIVATE,image->bytes_per_line*height,IPC_CREAT|0600);
            if(shared.shmid<0) throw std::runtime_error("cannot allocate XShm memory");
            shared.shmaddr=static_cast<char*>(shmat(shared.shmid,nullptr,0));
            if(shared.shmaddr==reinterpret_cast<char*>(-1)) throw std::runtime_error("cannot map XShm memory");
            image->data=shared.shmaddr;shared.readOnly=False;
            if(!XShmAttach(display,&shared)) throw std::runtime_error("cannot attach XShm memory");
            attached=true;XSync(display,False);
            // Remove the segment automatically after the last attachment is released.
            shmctl(shared.shmid,IPC_RMID,nullptr);
            pixels.resize(static_cast<size_t>(width)*height*4);
        } catch(...) {release();throw;}
    }
    ~FrameCapture(){release();}
    FrameCapture(const FrameCapture&)=delete;
    FrameCapture& operator=(const FrameCapture&)=delete;
    int GetWidth() const {return width;}
    int GetHeight() const {return height;}
    // Scheduling belongs to the caller; each call captures one image.
    const uint8_t* CaptureFrame() {
        if(!XShmGetImage(display,root,image,0,0,AllPlanes)) throw std::runtime_error("XShm capture failed");
        for(int y=0;y<height;++y)
            std::memcpy(pixels.data()+static_cast<size_t>(y)*width*4,image->data+y*image->bytes_per_line,width*4);
        return pixels.data();
    }
};
