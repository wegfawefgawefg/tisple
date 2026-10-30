// tisple disk: DOS 3.3 images (new, list, put, get, delete) and self-booting disks (boot)

#include "disk/disk_command.hpp"

#include "disk/boot_loader.hpp"
#include "disk/dos33.hpp"
#include "disk/image.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace tisple {

namespace {

void PrintDiskUsage() {
    std::fprintf(stderr, "usage: tisple disk <command> <image> ...\n"
                         "  new IMAGE [--volume N]       an empty DOS 3.3 disk\n"
                         "  list IMAGE                   the catalog\n"
                         "  put IMAGE FILE [--name N] [--type B|T|A|I|S|R] [--addr A]\n"
                         "                               add a file: B (default) loads at --addr\n"
                         "                               (default $6000), T is host text\n"
                         "  get IMAGE NAME [-o FILE]     copy a file out (B without its header,\n"
                         "                               T as host text)\n"
                         "  delete IMAGE NAME\n"
                         "  boot IMAGE PROGRAM.bin [--origin A]   a disk that boots the program\n"
                         "images are .dsk / .do (DOS 3.3 order), .po (ProDOS order) or .nib;\n"
                         "numbers take $hex, 0xhex or decimal\n");
}

bool ParseNumber(const char* text, uint64_t& out) {
    char* end = nullptr;
    if (text[0] == '$') {
        out = std::strtoull(text + 1, &end, 16);
        return text[1] != '\0' && *end == '\0';
    }
    out = std::strtoull(text, &end, 0);
    return text[0] != '\0' && *end == '\0';
}

// the file's last path part, upper case, as a DOS name
std::string DosNameFor(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) {
        name = name.substr(0, dot);
    }
    for (char& c : name) {
        c = c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
    }
    return name.substr(0, 30);
}

int Fail(const std::string& message) {
    std::fprintf(stderr, "tisple disk: %s\n", message.c_str());
    return 1;
}

int DiskNew(int argc, char** argv) {
    if (argc < 1) {
        PrintDiskUsage();
        return 1;
    }
    uint64_t volume = kDefaultVolume;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--volume") == 0 && i + 1 < argc &&
            ParseNumber(argv[i + 1], volume) && volume >= 1 && volume <= 254) {
            i++;
        } else {
            PrintDiskUsage();
            return 1;
        }
    }
    std::unique_ptr<DiskSectors> disk = std::make_unique<DiskSectors>();
    FormatDos33(*disk, static_cast<uint8_t>(volume));
    std::string error;
    if (!WriteDiskSectorsFile(argv[0], *disk, error)) {
        return Fail(error);
    }
    std::printf("%s: empty DOS 3.3 disk, volume %u, %u sectors free\n", argv[0],
                static_cast<unsigned>(volume), DosFreeSectors(*disk));
    return 0;
}

int DiskList(int argc, char** argv) {
    if (argc != 1) {
        PrintDiskUsage();
        return 1;
    }
    std::unique_ptr<DiskSectors> disk = std::make_unique<DiskSectors>();
    std::string error;
    std::vector<DosFile> files;
    if (!ReadDiskSectorsFile(argv[0], *disk, error) || !DosCatalog(*disk, files, error)) {
        return Fail(error);
    }
    const uint8_t* vtoc = disk->data[17][0];
    std::printf("DISK VOLUME %u\n\n", vtoc[6]);
    for (const DosFile& f : files) {
        std::printf("%c%c %03u %s\n", f.locked ? '*' : ' ', f.type, f.sectors % 1000,
                    f.name.c_str());
    }
    std::printf("\n%u sectors free\n", DosFreeSectors(*disk));
    return 0;
}

int DiskPut(int argc, char** argv) {
    if (argc < 2) {
        PrintDiskUsage();
        return 1;
    }
    std::string name = DosNameFor(argv[1]);
    char type = 'B';
    uint64_t address = 0x6000;
    for (int i = 2; i < argc; i++) {
        bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--name") == 0 && has_value) {
            name = argv[++i];
        } else if (std::strcmp(argv[i], "--type") == 0 && has_value &&
                   std::strlen(argv[i + 1]) == 1) {
            type = static_cast<char>(argv[++i][0] & ~0x20);
        } else if (std::strcmp(argv[i], "--addr") == 0 && has_value &&
                   ParseNumber(argv[i + 1], address) && address <= 0xFFFF) {
            i++;
        } else {
            PrintDiskUsage();
            return 1;
        }
    }
    std::vector<uint8_t> data;
    if (!ReadWholeFile(argv[1], data)) {
        return Fail(std::string("cannot read '") + argv[1] + "'");
    }
    std::vector<uint8_t> contents = data;
    if (type == 'B') {
        if (data.size() > 0xFFFF) {
            return Fail("a binary file is at most 65535 bytes");
        }
        contents = DosBinaryContents(data, static_cast<uint16_t>(address));
    } else if (type == 'T') {
        contents = DosTextContents(std::string(data.begin(), data.end()));
    } else if (type == 'A') {
        contents.insert(contents.begin(), static_cast<uint8_t>((data.size() >> 8) & 0xFF));
        contents.insert(contents.begin(), static_cast<uint8_t>(data.size() & 0xFF));
    }
    std::unique_ptr<DiskSectors> disk = std::make_unique<DiskSectors>();
    std::string error;
    if (!ReadDiskSectorsFile(argv[0], *disk, error) ||
        !DosPutFile(*disk, name, type, contents, error) ||
        !WriteDiskSectorsFile(argv[0], *disk, error)) {
        return Fail(error);
    }
    std::printf("%s: put %c %s (%zu bytes)\n", argv[0], type, name.c_str(), data.size());
    return 0;
}

int DiskGet(int argc, char** argv) {
    if (argc < 2) {
        PrintDiskUsage();
        return 1;
    }
    std::string out = DosNameFor(argv[1]);
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else {
            PrintDiskUsage();
            return 1;
        }
    }
    std::unique_ptr<DiskSectors> disk = std::make_unique<DiskSectors>();
    std::string error;
    DosFile file;
    std::vector<uint8_t> contents;
    if (!ReadDiskSectorsFile(argv[0], *disk, error) ||
        !DosGetFile(*disk, argv[1], file, contents, error)) {
        return Fail(error);
    }
    std::vector<uint8_t> data = contents;
    char note[64] = "";
    if (file.type == 'B') {
        uint16_t address = 0;
        if (!DosBinaryData(contents, address, data)) {
            return Fail("'" + file.name + "' has a broken binary header");
        }
        std::snprintf(note, sizeof(note), ", loads at $%04X", address);
    } else if (file.type == 'T') {
        std::string text = DosTextData(contents);
        data.assign(text.begin(), text.end());
    } else if (file.type == 'A' && contents.size() >= 2) {
        size_t length = static_cast<size_t>(contents[0] | (contents[1] << 8));
        length = length + 2 > contents.size() ? contents.size() - 2 : length;
        data.assign(contents.begin() + 2, contents.begin() + 2 + static_cast<long>(length));
    }
    if (!WriteWholeFile(out, data)) {
        return Fail("cannot write '" + out + "'");
    }
    std::printf("%s: %c %s, %zu bytes%s\n", out.c_str(), file.type, file.name.c_str(), data.size(),
                note);
    return 0;
}

int DiskDelete(int argc, char** argv) {
    if (argc != 2) {
        PrintDiskUsage();
        return 1;
    }
    std::unique_ptr<DiskSectors> disk = std::make_unique<DiskSectors>();
    std::string error;
    if (!ReadDiskSectorsFile(argv[0], *disk, error) || !DosDeleteFile(*disk, argv[1], error) ||
        !WriteDiskSectorsFile(argv[0], *disk, error)) {
        return Fail(error);
    }
    std::printf("%s: deleted %s\n", argv[0], argv[1]);
    return 0;
}

int DiskBoot(int argc, char** argv) {
    if (argc < 2) {
        PrintDiskUsage();
        return 1;
    }
    uint64_t origin = 0x6000;
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--origin") == 0 && i + 1 < argc &&
            ParseNumber(argv[i + 1], origin) && origin <= 0xBFFF) {
            i++;
        } else {
            PrintDiskUsage();
            return 1;
        }
    }
    std::vector<uint8_t> program;
    if (!ReadWholeFile(argv[1], program)) {
        return Fail(std::string("cannot read '") + argv[1] + "'");
    }
    std::string error;
    BootDiskInfo info;
    MemorySegment main;
    main.address = static_cast<uint16_t>(origin);
    main.bytes = program;
    if (!WriteBootDisk(argv[0], {main}, main.address, info, error)) {
        return Fail(error);
    }
    return 0;
}

} // namespace

bool WriteBootDisk(const std::string& path, const std::vector<MemorySegment>& segments,
                   uint16_t entry, BootDiskInfo& info, std::string& error) {
    std::unique_ptr<DiskSectors> disk = std::make_unique<DiskSectors>();
    if (!MakeBootDisk(segments, entry, *disk, info, error) ||
        !WriteDiskSectorsFile(path, *disk, error)) {
        return false;
    }
    size_t bytes = 0;
    for (const MemorySegment& segment : segments) {
        bytes += segment.bytes.size();
    }
    std::string pieces =
        info.segments == 1 ? "" : " in " + std::to_string(info.segments) + " segments";
    std::printf("%s: boots %zu bytes%s from $%04X (%s); %u sectors free for DOS 3.3 files\n",
                path.c_str(), bytes, pieces.c_str(), entry, BootTracksText(info).c_str(),
                DosFreeSectors(*disk));
    return true;
}

int RunDiskCommand(int argc, char** argv) {
    if (argc < 3) {
        PrintDiskUsage();
        return 1;
    }
    const char* command = argv[2];
    int rest = argc - 3;
    char** args = argv + 3;
    if (std::strcmp(command, "new") == 0) {
        return DiskNew(rest, args);
    }
    if (std::strcmp(command, "list") == 0) {
        return DiskList(rest, args);
    }
    if (std::strcmp(command, "put") == 0) {
        return DiskPut(rest, args);
    }
    if (std::strcmp(command, "get") == 0) {
        return DiskGet(rest, args);
    }
    if (std::strcmp(command, "delete") == 0) {
        return DiskDelete(rest, args);
    }
    if (std::strcmp(command, "boot") == 0) {
        return DiskBoot(rest, args);
    }
    PrintDiskUsage();
    return 1;
}

} // namespace tisple
