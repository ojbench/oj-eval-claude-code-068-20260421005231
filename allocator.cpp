// allocator.cpp
// 该文件实现了基于TLSF算法的内存分配器

#include "allocator.hpp"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <new>

// 辅助函数：计算整数的对数（向下取整）
static inline int log2_floor(std::size_t x) {
    if (x == 0) return -1;
    int result = 0;
    while (x > 1) {
        x >>= 1;
        result++;
    }
    return result;
}

// 辅助函数：找到最低位的1
static inline int ffs(std::uint32_t x) {
    if (x == 0) return -1;
    int count = 0;
    while ((x & 1) == 0) {
        x >>= 1;
        count++;
    }
    return count;
}

// 辅助函数：找到16位整数最低位的1
static inline int ffs16(std::uint16_t x) {
    if (x == 0) return -1;
    int count = 0;
    while ((x & 1) == 0) {
        x >>= 1;
        count++;
    }
    return count;
}

TLSFAllocator::TLSFAllocator(std::size_t memoryPoolSize)
    : memoryPool(nullptr), poolSize(memoryPoolSize) {
    initializeMemoryPool(memoryPoolSize);
}

TLSFAllocator::~TLSFAllocator() {
    if (memoryPool) {
        delete[] static_cast<char*>(memoryPool);
    }
}

void TLSFAllocator::initializeMemoryPool(std::size_t size) {
    // 分配内存池
    memoryPool = new char[size];
    poolSize = size;

    // 初始化索引结构
    index.fliBitmap = 0;
    for (int i = 0; i < FLI_SIZE; i++) {
        index.sliBitmaps[i] = 0;
        for (int j = 0; j < SLI_SIZE; j++) {
            index.freeLists[i][j] = nullptr;
        }
    }

    // 创建初始的大块
    FreeBlock* initialBlock = static_cast<FreeBlock*>(memoryPool);
    initialBlock->data = memoryPool;
    initialBlock->size = size;
    initialBlock->isFree = true;
    initialBlock->prevPhysBlock = nullptr;
    initialBlock->nextPhysBlock = nullptr;
    initialBlock->prevFree = nullptr;
    initialBlock->nextFree = nullptr;

    // 将初始块插入空闲链表
    insertFreeBlock(initialBlock);
}

void TLSFAllocator::mappingFunction(std::size_t size, int& fli, int& sli) {
    if (size < (1ULL << FLI_BITS)) {
        fli = 0;
        sli = 0;
    } else {
        fli = log2_floor(size);
        std::size_t remainder = size - (1ULL << fli);
        std::size_t divisions = std::min((1ULL << fli), (1ULL << SLI_BITS));
        std::size_t step = (1ULL << fli) / divisions;
        sli = static_cast<int>(remainder / step);

        // 确保 sli 在有效范围内
        if (sli >= SLI_SIZE) {
            sli = SLI_SIZE - 1;
        }
    }

    // 确保 fli 在有效范围内
    if (fli >= FLI_SIZE) {
        fli = FLI_SIZE - 1;
    }
}

void TLSFAllocator::insertFreeBlock(FreeBlock* block) {
    int fli, sli;
    mappingFunction(block->size, fli, sli);

    // 插入到链表头部
    block->nextFree = index.freeLists[fli][sli];
    block->prevFree = nullptr;

    if (index.freeLists[fli][sli]) {
        index.freeLists[fli][sli]->prevFree = block;
    }

    index.freeLists[fli][sli] = block;

    // 更新位图
    index.fliBitmap |= (1U << fli);
    index.sliBitmaps[fli] |= (1U << sli);
}

void TLSFAllocator::removeFreeBlock(FreeBlock* block) {
    int fli, sli;
    mappingFunction(block->size, fli, sli);

    // 从链表中移除
    if (block->prevFree) {
        block->prevFree->nextFree = block->nextFree;
    } else {
        index.freeLists[fli][sli] = block->nextFree;
    }

    if (block->nextFree) {
        block->nextFree->prevFree = block->prevFree;
    }

    // 如果链表变空，更新位图
    if (index.freeLists[fli][sli] == nullptr) {
        index.sliBitmaps[fli] &= ~(1U << sli);
        if (index.sliBitmaps[fli] == 0) {
            index.fliBitmap &= ~(1U << fli);
        }
    }
}

TLSFAllocator::FreeBlock* TLSFAllocator::findSuitableBlock(std::size_t size) {
    int fli, sli;
    mappingFunction(size, fli, sli);

    // 首先尝试在当前 fli 级别找到合适的块
    std::uint16_t sliBitmap = index.sliBitmaps[fli] & (~0U << sli);
    if (sliBitmap) {
        int foundSli = ffs16(sliBitmap);
        if (foundSli != -1) {
            return index.freeLists[fli][foundSli];
        }
    }

    // 如果没有找到，在更高的 fli 级别查找
    std::uint32_t fliBitmap = index.fliBitmap & (~0U << (fli + 1));
    if (fliBitmap) {
        int foundFli = ffs(fliBitmap);
        if (foundFli != -1 && index.sliBitmaps[foundFli]) {
            int foundSli = ffs16(index.sliBitmaps[foundFli]);
            if (foundSli != -1) {
                return index.freeLists[foundFli][foundSli];
            }
        }
    }

    return nullptr;
}

void TLSFAllocator::splitBlock(FreeBlock* block, std::size_t size) {
    std::size_t remainingSize = block->size - size;

    // 只有当剩余大小足够时才分割
    if (remainingSize >= sizeof(FreeBlock)) {
        // 创建新的空闲块
        FreeBlock* newBlock = reinterpret_cast<FreeBlock*>(
            static_cast<char*>(block->data) + size);
        newBlock->data = newBlock;
        newBlock->size = remainingSize;
        newBlock->isFree = true;
        newBlock->prevPhysBlock = block;
        newBlock->nextPhysBlock = block->nextPhysBlock;
        newBlock->prevFree = nullptr;
        newBlock->nextFree = nullptr;

        // 更新原块
        block->size = size;
        block->nextPhysBlock = newBlock;

        // 更新物理上的下一块
        if (newBlock->nextPhysBlock) {
            newBlock->nextPhysBlock->prevPhysBlock = newBlock;
        }

        // 将新块插入空闲链表
        insertFreeBlock(newBlock);
    }
}

void TLSFAllocator::mergeAdjacentFreeBlocks(FreeBlock* block) {
    // 尝试与下一个块合并
    if (block->nextPhysBlock && block->nextPhysBlock->isFree) {
        FreeBlock* nextBlock = static_cast<FreeBlock*>(block->nextPhysBlock);

        // 从空闲链表中移除下一个块
        removeFreeBlock(nextBlock);

        // 合并
        block->size += nextBlock->size;
        block->nextPhysBlock = nextBlock->nextPhysBlock;

        if (block->nextPhysBlock) {
            block->nextPhysBlock->prevPhysBlock = block;
        }
    }

    // 尝试与前一个块合并
    if (block->prevPhysBlock && block->prevPhysBlock->isFree) {
        FreeBlock* prevBlock = static_cast<FreeBlock*>(block->prevPhysBlock);

        // 从空闲链表中移除当前块
        removeFreeBlock(block);

        // 从空闲链表中移除前一个块
        removeFreeBlock(prevBlock);

        // 合并
        prevBlock->size += block->size;
        prevBlock->nextPhysBlock = block->nextPhysBlock;

        if (prevBlock->nextPhysBlock) {
            prevBlock->nextPhysBlock->prevPhysBlock = prevBlock;
        }

        // 将合并后的块重新插入
        insertFreeBlock(prevBlock);
    } else {
        // 如果没有与前一个块合并，重新插入当前块
        insertFreeBlock(block);
    }
}

void* TLSFAllocator::allocate(std::size_t size) {
    if (size == 0) {
        return nullptr;
    }

    // 确保块大小至少能容纳 FreeBlock 结构
    std::size_t requiredSize = std::max(size, sizeof(FreeBlock));

    // 查找合适的块
    FreeBlock* block = findSuitableBlock(requiredSize);

    if (!block) {
        return nullptr; // 没有足够的内存
    }

    // 从空闲链表中移除
    removeFreeBlock(block);

    // 分割块（如果有剩余）
    splitBlock(block, requiredSize);

    // 标记为已使用
    block->isFree = false;

    return block->data;
}

void TLSFAllocator::deallocate(void* ptr) {
    if (!ptr) {
        return;
    }

    // 获取块头
    FreeBlock* block = static_cast<FreeBlock*>(ptr);

    // 标记为空闲
    block->isFree = true;

    // 合并相邻的空闲块
    mergeAdjacentFreeBlocks(block);
}

void* TLSFAllocator::getMemoryPoolStart() const {
    return memoryPool;
}

std::size_t TLSFAllocator::getMemoryPoolSize() const {
    return poolSize;
}

std::size_t TLSFAllocator::getMaxAvailableBlockSize() const {
    // 从最大的 FLI 开始查找
    for (int fli = FLI_SIZE - 1; fli >= 0; fli--) {
        if (index.fliBitmap & (1U << fli)) {
            // 找到最大的 SLI
            for (int sli = SLI_SIZE - 1; sli >= 0; sli--) {
                if (index.sliBitmaps[fli] & (1U << sli)) {
                    FreeBlock* block = index.freeLists[fli][sli];
                    std::size_t maxSize = 0;
                    while (block) {
                        maxSize = std::max(maxSize, block->size);
                        block = block->nextFree;
                    }
                    return maxSize;
                }
            }
        }
    }
    return 0;
}
