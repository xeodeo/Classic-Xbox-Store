#include "system/Files.h"

#include "XboxXtl.h"

#include <stdio.h>
#include <string.h>

namespace Files
{
bool exists(const char* path)
{
    return GetFileAttributesA(path) != (DWORD)-1;
}

bool readAll(const char* path, std::vector<unsigned char>& out)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(f, NULL);
    out.resize(size);
    DWORD got = 0;
    const BOOL ok = size == 0 || ReadFile(f, out.data(), size, &got, NULL);
    CloseHandle(f);
    return ok && got == size;
}

bool writeAll(const char* path, const void* data, size_t length)
{
    char temp[300];
    _snprintf(temp, sizeof(temp), "%s.tmp", path);
    temp[sizeof(temp) - 1] = 0;
    HANDLE f = CreateFileA(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(f, data, static_cast<DWORD>(length), &written, NULL);
    CloseHandle(f);
    if (!ok || written != length) {
        DeleteFileA(temp);
        return false;
    }
    DeleteFileA(path);
    return MoveFileA(temp, path) != 0;
}

bool writeDurable(const char* path, const void* data, size_t length)
{
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(f, data, static_cast<DWORD>(length), &written, NULL) && written == length &&
                    FlushFileBuffers(f);
    CloseHandle(f);
    return ok != 0;
}

void makeDirectories(const char* path)
{
    char partial[300];
    const size_t length = strlen(path);
    if (length >= sizeof(partial)) return;
    for (size_t i = 0; i <= length; ++i) {
        if (path[i] == '\\' || path[i] == 0) {
            memcpy(partial, path, i);
            partial[i] = 0;
            if (i > 2) CreateDirectoryA(partial, NULL);   // "T:" solo, no
        }
    }
}

const char* cacheRoot()
{
    return "T:\\cache";
}
} // namespace Files

Lock::Lock()
{
    CRITICAL_SECTION* cs = new CRITICAL_SECTION;
    InitializeCriticalSection(cs);
    m_cs = cs;
}

Lock::~Lock()
{
    CRITICAL_SECTION* cs = static_cast<CRITICAL_SECTION*>(m_cs);
    DeleteCriticalSection(cs);
    delete cs;
}

void Lock::enter()
{
    EnterCriticalSection(static_cast<CRITICAL_SECTION*>(m_cs));
}

void Lock::leave()
{
    LeaveCriticalSection(static_cast<CRITICAL_SECTION*>(m_cs));
}
