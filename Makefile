CXXFLAGS+=-O0 -g -Wall -Wextra -lpthread -lboost_filesystem -lboost_system

vulkan-cts-runner: src/vulkan-cts-runner.cc
	$(CXX) -o vulkan-cts-runner src/vulkan-cts-runner.cc $(CXXFLAGS)

clean:
	rm vulkan-cts-runner
