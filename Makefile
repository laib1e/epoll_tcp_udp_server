CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -O2
TARGET = server

all: $(TARGET)

$(TARGET): main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)