CXX = g++
CXXFLAGS = -std=c++20 -O3 -pthread -Wall

SENDER   = sender
RECEIVER = receiver
TEST_CTRL = test_controller

all: $(SENDER) $(RECEIVER)

$(SENDER): sender.cc pudica_algo.cc logger.cc protocol.h pudica_algo.h logger.h
	$(CXX) $(CXXFLAGS) sender.cc pudica_algo.cc logger.cc -o $(SENDER) -lX11 -lXext -lavcodec -lavutil -lswscale

$(RECEIVER): receiver.cc protocol.h
	$(CXX) $(CXXFLAGS) receiver.cc -o $(RECEIVER) -lSDL2 -lavcodec -lavutil -lswscale

$(TEST_CTRL): test_controller.cc pudica_algo.cc logger.cc protocol.h pudica_algo.h logger.h
	$(CXX) $(CXXFLAGS) test_controller.cc pudica_algo.cc logger.cc -o $(TEST_CTRL)

test: $(TEST_CTRL)
	./$(TEST_CTRL)

bench-quick:
	cd ../evaluations && python3 const_test.py --bw 20 --dur 30 --plot

bench:
	cd ../evaluations && python3 run_all_parallel.py

sift:
	@test -n "$(FILE)" || (echo "Usage: make sift FILE=<log.jsonl>" && exit 1)
	cd ../evaluations && python3 sift.py $(FILE)

clean:
	rm -f $(SENDER) $(RECEIVER) $(TEST_CTRL)