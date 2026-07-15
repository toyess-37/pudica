// Phase 1: capture_test.cc
// Depends on: capture.h, encoder.h
// Purpose: Standalone executable that captures 10 seconds of X11 frames and encodes to H.264

#include "capture.h"
#include "encoder.h"
#include <fstream>
#include <iostream>
#include <numeric>
#include <chrono>

int main(int argc, char** argv) {
    try {
        std::cout << "Initializing Capture on :99..." << std::endl;
        FrameCapture capture(":99");
        int width = capture.GetWidth();
        int height = capture.GetHeight();
        std::cout << "Capture initialized. Resolution: " << width << "x" << height << std::endl;

        std::cout << "Initializing H.264 Encoder at 35 FPS..." << std::endl;
        VideoEncoder encoder(width, height, 35);
        std::cout << "Encoder initialized." << std::endl;

        std::ofstream outfile("output.h264", std::ios::binary);
        if (!outfile) {
            std::cerr << "Failed to open output.h264 for writing" << std::endl;
            return 1;
        }

        const int target_frames = 35 * 10; // 10 seconds at 35 fps
        std::vector<double> capture_times;
        std::vector<double> encode_times;

        std::cout << "Starting 10-second capture loop..." << std::endl;

        for (int i = 0; i < target_frames; ++i) {
            auto start_cap = std::chrono::high_resolution_clock::now();
            const uint8_t* bgra_data = capture.CaptureFrame();
            auto end_cap = std::chrono::high_resolution_clock::now();
            
            auto start_enc = std::chrono::high_resolution_clock::now();
            auto packets = encoder.Encode(bgra_data);
            auto end_enc = std::chrono::high_resolution_clock::now();

            for (const auto& pkt_data : packets) {
                outfile.write(reinterpret_cast<const char*>(pkt_data.data()), pkt_data.size());
            }

            double cap_time_ms = std::chrono::duration<double, std::milli>(end_cap - start_cap).count();
            double enc_time_ms = std::chrono::duration<double, std::milli>(end_enc - start_enc).count();
            
            capture_times.push_back(cap_time_ms);
            encode_times.push_back(enc_time_ms);

            if (i % 35 == 0) {
                std::cout << "Captured " << i << " / " << target_frames << " frames" << std::endl;
            }
        }

        // Flush encoder
        auto flush_packets = encoder.Encode(nullptr);
        for (const auto& pkt_data : flush_packets) {
            outfile.write(reinterpret_cast<const char*>(pkt_data.data()), pkt_data.size());
        }

        double avg_cap = std::accumulate(capture_times.begin(), capture_times.end(), 0.0) / capture_times.size();
        double avg_enc = std::accumulate(encode_times.begin(), encode_times.end(), 0.0) / encode_times.size();

        std::cout << "Capture complete! Wrote output.h264" << std::endl;
        std::cout << "Average capture time per frame: " << avg_cap << " ms" << std::endl;
        std::cout << "Average encode time per frame:  " << avg_enc << " ms" << std::endl;
        
        // At 35fps, we have ~28.57ms budget per frame.
        std::cout << "Total pipeline time per frame:  " << (avg_cap + avg_enc) << " ms (Budget: 28.57 ms)" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
