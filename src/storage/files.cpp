#include "files.h"
#include "../../include/pins.h"
#include <SD.h>
#include <LittleFS.h>
#include <SPI.h>
#include <vector>
#include <esp_heap_caps.h>

namespace {
    bool sdOk = false;
    bool flashOk = false;

    // Аллокатор в PSRAM: кэш каталога может быть большим (сотни записей по
    // 72 байта), внутренний heap дороже — держим массив записей во внешней
    // памяти. Если PSRAM вдруг недоступна — fallback в обычный malloc.
    template <typename T>
    struct PsramAlloc {
        typedef T value_type;
        PsramAlloc() = default;
        template <typename U> PsramAlloc(const PsramAlloc<U>&) {}
        T* allocate(std::size_t n) {
            void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!p) p = malloc(n * sizeof(T));
            return static_cast<T*>(p);
        }
        void deallocate(T* p, std::size_t) { free(p); }
    };
    template <typename T, typename U>
    bool operator==(const PsramAlloc<T>&, const PsramAlloc<U>&) { return true; }
    template <typename T, typename U>
    bool operator!=(const PsramAlloc<T>&, const PsramAlloc<U>&) { return false; }

    struct Cache {
        FsRoot root = FsRoot::SD;
        String path;
        std::vector<FsEntry, PsramAlloc<FsEntry>> entries;
        bool valid = false;
        uint32_t atMs = 0; // момент последнего скана — TTL на случай внешних изменений
    } cache;

    // Кэш протухает: явная инвалидация из WebUI (upload/delete/create)
    // плюс TTL-страховка для подмены SD-карты вручную.
    constexpr uint32_t CACHE_TTL_MS = 10000;

    bool cacheStale(FsRoot root, const String& path) {
        return !cache.valid || cache.root != root || cache.path != path ||
               millis() - cache.atMs > CACHE_TTL_MS;
    }

    void rebuildCache(FsRoot root, const String& path) {
        cache.entries.clear();
        cache.root = root;
        cache.path = path;
        cache.valid = true;
        cache.atMs = millis();

        fs::FS& fsRef = (root == FsRoot::SD) ? (fs::FS&)SD : (fs::FS&)LittleFS;
        File dir = fsRef.open(path.length() ? path : "/");
        if (!dir || !dir.isDirectory()) return;

        File f = dir.openNextFile();
        while (f) {
            FsEntry e;
            const char* fn = f.name(); // SD может вернуть имя с ведущим '/'
            const char* slash = strrchr(fn, '/');
            strncpy(e.name, slash ? slash + 1 : fn, sizeof(e.name) - 1);
            e.name[sizeof(e.name) - 1] = 0;
            e.isDir = f.isDirectory();
            e.size = f.size();
            cache.entries.push_back(e);
            f = dir.openNextFile();
        }
    }
}

bool Files::beginSD() {
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    sdOk = SD.begin(PIN_SD_CS);
    return sdOk;
}

bool Files::beginFlash() {
    flashOk = LittleFS.begin(true);
    return flashOk;
}

bool Files::sdPresent() { return sdOk; }
bool Files::flashPresent() { return flashOk; }
uint64_t Files::sdTotalBytes() { return sdOk ? SD.totalBytes() : 0; }
uint64_t Files::sdUsedBytes()  { return sdOk ? SD.usedBytes()  : 0; }

void Files::invalidateCache() { cache.valid = false; }

int Files::listCount(FsRoot root, const String& path) {
    if ((root == FsRoot::SD && !sdOk) || (root == FsRoot::Flash && !flashOk)) return 0;
    if (cacheStale(root, path)) rebuildCache(root, path);
    return (int)cache.entries.size();
}

FsEntry Files::listEntry(FsRoot root, const String& path, int index) {
    if (cacheStale(root, path)) rebuildCache(root, path);
    if (index < 0 || index >= (int)cache.entries.size()) return FsEntry{"?", false, 0};
    return cache.entries[index];
}
