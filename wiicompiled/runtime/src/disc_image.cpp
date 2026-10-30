#include "disc_image.h"

#include <cryptopp/aes.h>
#include <cryptopp/modes.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace DiscImage {
namespace {
namespace fs = std::filesystem;
std::string U8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// Retail Wii common key (index 0).
constexpr uint8_t kCommonKey[16] = {0xEB, 0xE4, 0x2A, 0x22, 0x5E, 0x85, 0x93, 0xE4,
                                    0x48, 0xD9, 0xC5, 0x45, 0x73, 0x81, 0xAA, 0xF7};
constexpr int64_t kCluster = 0x8000, kClusterData = 0x7C00;

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

void DecryptCbc(const uint8_t* key, const uint8_t* iv, const uint8_t* in, uint8_t* out, size_t size) {
    CryptoPP::CBC_Mode<CryptoPP::AES>::Decryption aes;
    aes.SetKeyWithIV(key, 16, iv, 16);
    aes.ProcessData(out, in, size);
}

// The raw (still encrypted) disc bytes behind each container format; blocks an image does not store read as zeros.
class Container {
public:
    explicit Container(const fs::path& path) : file_(path, std::ios::binary) {
        if (!file_) throw std::runtime_error("The file can't be opened.");
        char magic[4] = {};
        file_.read(magic, 4);
        if (std::memcmp(magic, "WBFS", 4) == 0) {
            uint8_t h[512] = {};
            ReadFile(0, h, sizeof h);
            const int64_t hdSec = int64_t(1) << h[8];
            block_ = int64_t(1) << h[9];
            const int64_t n = 143432LL * 2 * 0x8000 / block_;  // covers a dual-layer disc
            std::vector<uint8_t> table(size_t(0x100 + n * 2));
            ReadFile(hdSec, table.data(), table.size());
            map_.resize(size_t(n));
            for (int64_t i = 0; i < n; ++i) map_[size_t(i)] = Be16(&table[size_t(0x100 + i * 2)]);
            kind_ = Kind::Wbfs;
        } else if (std::memcmp(magic, "CISO", 4) == 0) {
            std::vector<uint8_t> h(0x8000);
            ReadFile(0, h.data(), h.size());
            block_ = Le32(&h[4]);
            if (block_ < 0x8000 || block_ % 0x8000 != 0) throw std::runtime_error("Unsupported CISO block size.");
            map_.resize(0x8000 - 8);
            for (size_t i = 0, k = 0; i < map_.size(); ++i) map_[i] = h[8 + i] == 1 ? uint32_t(++k) : 0;  // 1-based
            kind_ = Kind::Ciso;
        }
    }

    void Read(int64_t off, uint8_t* dst, size_t size) {
        if (kind_ == Kind::Iso) {
            ReadFile(off, dst, size);
            return;
        }
        while (size > 0) {
            const int64_t i = off / block_, o = off % block_;
            const size_t n = size_t(std::min<int64_t>(int64_t(size), block_ - o));
            const uint32_t slot = size_t(i) < map_.size() ? map_[size_t(i)] : 0;
            if (slot == 0) std::memset(dst, 0, n);
            else if (kind_ == Kind::Wbfs) ReadFile(int64_t(slot) * block_ + o, dst, n);
            else ReadFile(0x8000 + int64_t(slot - 1) * block_ + o, dst, n);
            off += int64_t(n);
            dst += n;
            size -= n;
        }
    }

private:
    enum class Kind { Iso, Ciso, Wbfs };
    void ReadFile(int64_t off, uint8_t* dst, size_t size) {
        file_.clear();
        file_.seekg(off);
        file_.read(reinterpret_cast<char*>(dst), std::streamsize(size));
        const size_t got = size_t(std::max<std::streamsize>(0, file_.gcount()));
        if (got < size) std::memset(dst + got, 0, size - got);  // past the end of a truncated image
    }
    std::ifstream file_;
    Kind kind_ = Kind::Iso;
    int64_t block_ = 0;
    std::vector<uint32_t> map_;
};

class Disc {
public:
    explicit Disc(const fs::path& path) : disc_(path) {
        uint8_t header[0x60] = {};
        disc_.Read(0, header, sizeof header);
        info.id.assign(reinterpret_cast<char*>(header), 6);
        info.revision = header[7];
        info.title.assign(reinterpret_cast<char*>(header + 0x20), strnlen(reinterpret_cast<char*>(header + 0x20), 0x40));
        if (!std::all_of(info.id.begin(), info.id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'); }))
            throw std::runtime_error("This file is not a Wii disc image.");
        // Partition table: four groups of (count, table offset >> 2); type 0 is the game data.
        int64_t partition = -1;
        for (int g = 0; g < 4 && partition < 0; ++g) {
            uint8_t group[8];
            disc_.Read(0x40000 + g * 8, group, 8);
            const uint32_t count = Be32(group), table = Be32(group + 4);
            for (uint32_t i = 0; i < count && i < 64; ++i) {
                uint8_t entry[8];
                disc_.Read((int64_t(table) << 2) + i * 8, entry, 8);
                if (Be32(entry + 4) == 0) {
                    partition = int64_t(Be32(entry)) << 2;
                    break;
                }
            }
        }
        if (partition < 0) throw std::runtime_error("The image has no game data partition.");
        uint8_t ticket[0x2C0];
        disc_.Read(partition, ticket, sizeof ticket);
        if (ticket[0x1F1] != 0) throw std::runtime_error("Only discs using common key 0 are supported.");
        uint8_t iv[16] = {};
        std::memcpy(iv, ticket + 0x1DC, 8);
        DecryptCbc(kCommonKey, iv, ticket + 0x1BF, titleKey_, 16);
        dataBase_ = partition + (int64_t(Be32(ticket + 0x2B8)) << 2);
        uint8_t boot[0x20];
        ReadData(0, boot, sizeof boot);
        if (std::memcmp(boot, info.id.data(), 6) != 0 || Be32(boot + 0x18) != 0x5D1C9EA3u)
            throw std::runtime_error("The game data did not decrypt: the image is damaged or not a plain Wii disc.");
    }

    // Bytes of the decrypted game partition.
    void ReadData(int64_t off, uint8_t* dst, size_t size) {
        while (size > 0) {
            const int64_t ci = off / kClusterData, o = off % kClusterData;
            if (ci != cached_) {
                std::vector<uint8_t> raw(static_cast<size_t>(kCluster));
                disc_.Read(dataBase_ + ci * kCluster, raw.data(), raw.size());
                plain_.resize(size_t(kClusterData));
                DecryptCbc(titleKey_, &raw[0x3D0], &raw[0x400], plain_.data(), plain_.size());
                cached_ = ci;
            }
            const size_t n = size_t(std::min<int64_t>(int64_t(size), kClusterData - o));
            std::memcpy(dst, &plain_[size_t(o)], n);
            dst += n;
            off += int64_t(n);
            size -= n;
        }
    }

    Info info;

private:
    Container disc_;
    uint8_t titleKey_[16] = {};
    int64_t dataBase_ = 0, cached_ = -1;
    std::vector<uint8_t> plain_;
};
} // namespace

bool Probe(const fs::path& image, Info& info, std::string& error) {
    try {
        Disc disc(image);
        info = disc.info;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool Extract(const fs::path& image, const fs::path& outDir, const std::function<void(int, int)>& progress, std::string& error) {
    try {
        Disc disc(image);
        uint8_t boot[0x440];
        disc.ReadData(0, boot, sizeof boot);
        const int64_t dolOff = int64_t(Be32(boot + 0x420)) << 2, fstOff = int64_t(Be32(boot + 0x424)) << 2,
                      fstSize = int64_t(Be32(boot + 0x428)) << 2;
        uint8_t dolHeader[0x100];
        disc.ReadData(dolOff, dolHeader, sizeof dolHeader);
        int64_t dolSize = 0;
        for (int i = 0; i < 18; ++i)
            if (Be32(dolHeader + 0x90 + 4 * i) != 0)
                dolSize = std::max<int64_t>(dolSize, int64_t(Be32(dolHeader + 4 * i)) + Be32(dolHeader + 0x90 + 4 * i));
        uint8_t apl[0x20];
        disc.ReadData(0x2440, apl, sizeof apl);
        const int64_t aplSize = 0x20 + int64_t(Be32(apl + 0x14)) + Be32(apl + 0x18);
        std::vector<uint8_t> fst(size_t(fstSize) + 1, 0);
        disc.ReadData(fstOff, fst.data(), size_t(fstSize));
        const uint32_t entries = Be32(&fst[8]);
        const size_t nameBase = size_t(entries) * 12;
        if (nameBase >= fst.size()) throw std::runtime_error("The disc's file table is damaged.");

        struct File { fs::path path; int64_t off, size; };
        std::vector<File> files;
        // FST: directories hold the index one past their last entry; files their offset >> 2 and size.
        std::vector<std::pair<uint32_t, fs::path>> dirs{{entries, fs::path("files")}};
        for (uint32_t i = 1; i < entries; ++i) {
            while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
            const uint8_t* e = &fst[size_t(i) * 12];
            const size_t nameOff = nameBase + ((size_t(e[1]) << 16) | (size_t(e[2]) << 8) | e[3]);
            if (nameOff >= fst.size()) throw std::runtime_error("The disc's file table is damaged.");
            const std::string name(reinterpret_cast<const char*>(&fst[nameOff]));
            if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos)
                throw std::runtime_error("The disc's file table has a bad name.");
            if (e[0] != 0) dirs.emplace_back(Be32(e + 8), dirs.back().second / fs::u8path(name));
            else files.push_back({dirs.back().second / fs::u8path(name), int64_t(Be32(e + 4)) << 2, Be32(e + 8)});
        }

        const int total = int(files.size()) + 5;
        int done = 0;
        std::vector<uint8_t> buffer(1 << 20);
        auto put = [&](const fs::path& rel, int64_t off, int64_t size) {
            const fs::path path = outDir / rel;
            fs::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("Can't write " + U8(path));
            for (int64_t o = 0; o < size; o += int64_t(buffer.size())) {
                const size_t n = size_t(std::min<int64_t>(int64_t(buffer.size()), size - o));
                disc.ReadData(off + o, buffer.data(), n);
                out.write(reinterpret_cast<const char*>(buffer.data()), std::streamsize(n));
            }
            if (!out) throw std::runtime_error("Can't write " + U8(path) + " (is the drive full?)");
            if (progress) progress(++done, total);
        };
        put(fs::path("sys") / "boot.bin", 0, 0x440);
        put(fs::path("sys") / "bi2.bin", 0x440, 0x2000);
        put(fs::path("sys") / "apploader.img", 0x2440, aplSize);
        put(fs::path("sys") / "main.dol", dolOff, dolSize);
        put(fs::path("sys") / "fst.bin", fstOff, fstSize);
        for (const File& f : files) put(f.path, f.off, f.size);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
} // namespace DiscImage
