CXX = g++
CXXFLAGS = -std=c++20 -O3 -pthread -Wall

SENDER = sender
RECEIVER = receiver

all: $(SENDER) $(SENDER_KERN) $(RECEIVER)

$(SENDER): sender.cpp pudica_algo.cpp protocol.h pudica_algo.h
	$(CXX) $(CXXFLAGS) sender.cpp pudica_algo.cpp -o $(SENDER)

$(RECEIVER): receiver.cpp protocol.h
	$(CXX) $(CXXFLAGS) receiver.cpp -o $(RECEIVER)

clean:
	rm -f $(SENDER) $(RECEIVER)