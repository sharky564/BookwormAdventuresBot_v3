CXX      := clang++
SRC      := trie.cpp rack.cpp main.cpp
TARGET   := solver

CXXFLAGS := -std=c++23 -Wall -Wextra

$(TARGET): $(SRC) $(wildcard *.hpp)
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG -march=native -pthread -Ithird_party -flto -fuse-ld=lld $(SRC) -o $@ -lstdc++exp

static: $(SRC) $(wildcard *.hpp)
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG -march=native -pthread -Ithird_party -flto -fuse-ld=lld -static-libgcc -static-libstdc++ $(SRC) -o $(TARGET) -lstdc++exp

debug:
	$(CXX) $(CXXFLAGS) -O0 -pthread -Ithird_party -g $(SRC) -o $(TARGET)_dbg -lstdc++exp

profile:
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG -march=native -g -fno-omit-frame-pointer -pthread -Ithird_party $(SRC) -o $(TARGET)_profile -lstdc++exp

bench: trie.cpp rack.cpp bench.cpp $(wildcard *.hpp)
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG -march=native -pthread -Ithird_party -flto -fuse-ld=lld trie.cpp rack.cpp bench.cpp -o bench -lstdc++exp

clean:
	rm -f $(TARGET) $(TARGET)_dbg $(TARGET)_profile bench