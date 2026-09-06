#ifndef _RESOURCE_MANAGER_H_
#define _RESOURCE_MANAGER_H_

#include <cstdint>
#include <cstddef>

class ResourceManager {
public:
    static ResourceManager& GetInstance() {
        static ResourceManager instance;
        return instance;
    }

    void PrintSystemMemoryInfo();
    size_t GetFreePsramSize() const;
    size_t GetFreeInternalHeapSize() const;

private:
    ResourceManager() = default;
};

#endif // _RESOURCE_MANAGER_H_
