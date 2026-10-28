#pragma once
#include <Arduino.h>

enum class FsRoot { SD, Flash };

struct FsEntry {
    // Фиксированный буфер вместо String: кэш каталога держит сотни записей,
    // и каждая String отъедала бы внутренний heap. 64 байт хватает с запасом
    // для отображения на экране 320px; более длинные имена обрезаются.
    char name[64];
    bool isDir;
    size_t size;
};

namespace Files {
    bool beginSD();
    bool beginFlash();

    int listCount(FsRoot root, const String& path);
    FsEntry listEntry(FsRoot root, const String& path, int index);
    void invalidateCache(); // после изменений извне (WebUI: upload/delete/create)

    bool sdPresent();
    bool flashPresent();
    uint64_t sdTotalBytes();
    uint64_t sdUsedBytes();
}
