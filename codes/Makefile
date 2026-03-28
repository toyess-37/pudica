CXX = g++
CXXFLAGS = -std=c++20 -O3 -pthread -Wall

SENDER = sender
RECEIVER = receiver

all: $(SENDER) $(RECEIVER)

$(SENDER): sender.cc pudica_algo.cc protocol.h pudica_algo.h
	$(CXX) $(CXXFLAGS) sender.cc pudica_algo.cc -o $(SENDER)

$(RECEIVER): receiver.cc protocol.h
	$(CXX) $(CXXFLAGS) receiver.cc -o $(RECEIVER)

clean:
	rm -f $(SENDER) $(RECEIVER)