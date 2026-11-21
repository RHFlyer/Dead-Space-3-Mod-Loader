#pragma once

#include <windows.h>

#include <cstdint>

// Reads a DLL file's own bytes, without mapping or running it: whether it is a 32-bit DLL, what it exports and
// imports, and its SHA-256. Every offset taken from the file is checked against the file's size, so a cut-off or
// made-up file gives "not an image", never a bad read.
class PeFile {
public:
    explicit PeFile(const wchar_t* path);
    ~PeFile();
    PeFile(const PeFile&) = delete;
    PeFile& operator=(const PeFile&) = delete;

    bool Read() const { return data_ != nullptr; }  // the whole file was read (it is at most kMaxBytes)
    bool Image() const { return image_; }           // a well-formed PE image
    bool X86Dll() const { return image_ && x86_ && dll_; }
    unsigned long long Size() const { return size_; }

    // Whether the export table names `name`.
    bool Exports(const char* name) const;
    // The DLLs the import table names, in file order: calls each(name, user) until it returns false.
    void Imports(bool (*each)(const char* name, void* user), void* user) const;
    // 64 hex digits and a 0; empty if the file was not read.
    void Sha256(char out[65]) const;

    static constexpr unsigned long long kMaxBytes = 64ull * 1024 * 1024;

private:
    // The file offset of `size` bytes at relative virtual address `rva`, or nullptr if they are not in the file.
    const uint8_t* At(uint32_t rva, uint32_t size) const;
    // The 0-terminated text at `rva` (at most 255 characters), or nullptr.
    const char* TextAt(uint32_t rva) const;

    uint8_t* data_ = nullptr;
    unsigned long long size_ = 0;
    bool image_ = false, x86_ = false, dll_ = false;
    const uint8_t* sections_ = nullptr;  // IMAGE_SECTION_HEADER[sectionCount_], inside data_
    unsigned sectionCount_ = 0;
    uint32_t exportRva_ = 0, exportSize_ = 0, importRva_ = 0, importSize_ = 0;
    uint32_t headers_ = 0;  // SizeOfHeaders: below it an address is its own file offset
};
