# Phase 1: Makefile for capture test

CXX = g++
CXXFLAGS = -O3 -Wall -std=c++17 -I.
LDFLAGS = -lX11 -lXext -lavcodec -lavutil -lswscale -lpthread

TARGET = capture_test
SRCS = capture_test.cc

all: $(TARGET)

$(TARGET): $(SRCS) capture.h encoder.h
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) output.h264
