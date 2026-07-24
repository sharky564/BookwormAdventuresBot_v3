CXX      := clang++
SRC      := trie.cpp rack.cpp main.cpp
TARGET   := solver

CXXFLAGS := -std=c++23 -Wall -Wextra

# -static links libwinpthread/libstdc++/libgcc into the exe. Without it the
# binary picks up whichever libwinpthread-1.dll is first on PATH (Git Bash
# shadows msys2's with an incompatible copy -> intermittent thread-teardown
# crashes). A fully static exe is PATH-independent.
$(TARGET): $(SRC) $(wildcard *.hpp)
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG -march=native -pthread -Ithird_party -flto -fuse-ld=lld -static $(SRC) -o $@ -lstdc++exp

static: $(SRC) $(wildcard *.hpp)
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG -march=native -pthread -Ithird_party -flto -fuse-ld=lld -static $(SRC) -o $(TARGET) -lstdc++exp

debug:
	$(CXX) $(CXXFLAGS) -O0 -pthread -Ithird_party -g $(SRC) -o $(TARGET)_dbg -lstdc++exp

profile:
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG -march=native -g -fno-omit-frame-pointer -pthread -Ithird_party $(SRC) -o $(TARGET)_profile -lstdc++exp

bench: trie.cpp rack.cpp bench.cpp $(wildcard *.hpp)
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG -march=native -pthread -Ithird_party -flto -fuse-ld=lld -static trie.cpp rack.cpp bench.cpp -o bench -lstdc++exp

# Profile-guided optimization. Requires BOTH:
#   pacman -S mingw-w64-ucrt-x86_64-llvm          (llvm-profdata)
#   pacman -S mingw-w64-ucrt-x86_64-compiler-rt   (libclang_rt.profile)
# If the link fails with "could not open ...x86_64-w64-windows-gnu/
# libclang_rt.profile.a", the compiler-rt package used the legacy layout;
# bridge it (redo after clang major updates):
#   mkdir -p /ucrt64/lib/clang/<N>/lib/x86_64-w64-windows-gnu
#   cp /ucrt64/lib/clang/<N>/lib/windows/libclang_rt.profile-x86_64.a \
#      /ucrt64/lib/clang/<N>/lib/x86_64-w64-windows-gnu/libclang_rt.profile.a
# Workflow:
#   make pgo-gen
#   ./solver_pgogen ../game_data/ba1-dictionary.txt gen-rack 500
#   ./solver_pgogen ../game_data/ba1-dictionary.txt serve < baseline_sim_req.jsonl > /dev/null
#   llvm-profdata merge -output=default.profdata *.profraw
#   make pgo
pgo-gen: $(SRC) $(wildcard *.hpp)
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG -march=native -pthread -Ithird_party -flto -fuse-ld=lld -static -fprofile-generate $(SRC) -o $(TARGET)_pgogen -lstdc++exp

pgo: $(SRC) $(wildcard *.hpp) default.profdata
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG -march=native -pthread -Ithird_party -flto -fuse-ld=lld -static -fprofile-use=default.profdata $(SRC) -o $(TARGET) -lstdc++exp

clean:
	rm -f $(TARGET) $(TARGET)_dbg $(TARGET)_profile $(TARGET)_pgogen bench *.profraw default.profdata