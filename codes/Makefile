CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -pthread -Wall -Wextra
VIDEO_CAPTURE = ../cloud_gaming/capture/capture.h ../cloud_gaming/capture/encoder.h
VIDEO_PLAYBACK = ../cloud_gaming/playback/decoder.h ../cloud_gaming/playback/renderer.h
.PHONY: all test integration bench bench-quick sift clean
all: sender receiver

sender: sender.cc pudica_algo.cc logger.cc protocol.h transport.h pudica_algo.h logger.h $(VIDEO_CAPTURE)
	$(CXX) $(CXXFLAGS) sender.cc pudica_algo.cc logger.cc -o $@ -lX11 -lXext -lavcodec -lavutil -lswscale

receiver: receiver.cc protocol.h transport.h $(VIDEO_PLAYBACK)
	$(CXX) $(CXXFLAGS) receiver.cc -o $@ -lSDL2 -lavcodec -lavutil -lswscale

test_controller: test_controller.cc pudica_algo.cc logger.cc protocol.h pudica_algo.h logger.h
	$(CXX) $(CXXFLAGS) test_controller.cc pudica_algo.cc logger.cc -o $@

test_transport: test_transport.cc transport.h protocol.h
	$(CXX) $(CXXFLAGS) test_transport.cc -o $@

test: test_controller test_transport
	./test_controller
	./test_transport
	cd .. && python3 -m unittest discover -s tests -p 'test_*.py'

integration: all
	cd .. && python3 tests/integration_transport.py

bench-quick: all
	cd ../evaluations && python3 const_test.py --bw 20 --dur 10

bench: all
	cd ../evaluations && python3 run_all.py

sift:
	@test -n "$(FILE)" || (echo 'Usage: make sift FILE=<log.jsonl>' && exit 1)
	python3 ../evaluations/sift.py --log "$(abspath $(FILE))"

clean:
	rm -f sender receiver test_controller test_transport
