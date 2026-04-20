// main.cpp
// TLSF Allocator test driver

#include "allocator.hpp"
#include <iostream>
#include <string>
#include <vector>

int main() {
    std::size_t poolSize;
    std::cin >> poolSize;

    TLSFAllocator allocator(poolSize);

    int numOperations;
    std::cin >> numOperations;

    std::vector<void*> allocatedBlocks;

    for (int i = 0; i < numOperations; i++) {
        std::string operation;
        std::cin >> operation;

        if (operation == "allocate") {
            std::size_t size;
            std::cin >> size;
            void* ptr = allocator.allocate(size);
            allocatedBlocks.push_back(ptr);
            if (ptr) {
                std::cout << "Allocated " << size << " bytes at " << ptr << std::endl;
            } else {
                std::cout << "Failed to allocate " << size << " bytes" << std::endl;
            }
        } else if (operation == "deallocate") {
            int index;
            std::cin >> index;
            if (index >= 0 && index < static_cast<int>(allocatedBlocks.size())) {
                allocator.deallocate(allocatedBlocks[index]);
                std::cout << "Deallocated block at index " << index << std::endl;
            }
        } else if (operation == "max_available") {
            std::size_t maxSize = allocator.getMaxAvailableBlockSize();
            std::cout << "Max available block size: " << maxSize << std::endl;
        } else if (operation == "pool_size") {
            std::cout << "Pool size: " << allocator.getMemoryPoolSize() << std::endl;
        }
    }

    return 0;
}
