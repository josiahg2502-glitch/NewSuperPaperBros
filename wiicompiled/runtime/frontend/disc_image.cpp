#include "disc_image.h"
#include <cryptopp/aes.h>
#include <cryptopp/modes.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace RuntimeFrontend::DiscImage {
namespace {
// Retail Wii common key (index 0), as in tools/extract_wbfs.py and launcher/WiiDisc.cs.
constexpr uint8_t kCommonKey[16] = {0xEB, 0xE4, 0x2A, 0x22, 0x5E, 0x85, 0x93, 0xE4,
                                    0x48, 0xD9, 0xC5, 0x45, 0x73, 0x81, 0xAA, 0xF7};
constexpr uint64_t kCluster = 0x8000, kClusterData = 0x7C00;
// Bump when the extracted layout changes so older caches are rebuilt.
constexpr const char* kCacheFormat = "spm-disc-cache 1";
constexpr const char* kMarker = ".spm-disc";
constexpr unsigned kThreads = 4;  // storage, not CPU, limits beyond this

uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t U16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

struct FileCloser { void operator()(std::FILE* f) const { if (f) std::fclose(f); } };
using File = std::unique_ptr<std::FILE, FileCloser>;

File Open(const fs::path& path, const char* mode) {
    File file(std::fopen(path.c_str(), mode));
    if (!file) throw std::runtime_error("Cannot open " + path.string());
    return file;
}

class Wbfs {
public:
    explicit Wbfs(const fs::path& path) : file(Open(path, "rb")) {
        uint8_t header[512];
        if (std::fread(header, 1, sizeof(header), file.get()) != sizeof(header) || std::memcmp(header, "WBFS", 4) != 0)
            throw std::runtime_error("Not a WBFS disc image");
        if (header[8] < 9 || header[8] > 16 || header[9] < 15 || header[9] > 30)
            throw std::runtime_error("Unsupported WBFS sector sizes");
        const uint64_t hdSector = uint64_t(1) << header[8];
        block = uint64_t(1) << header[9];
        const size_t count = size_t(143432ull * 2 * kCluster / block);  // covers a dual-layer disc
        std::vector<uint8_t> table(0x100 + count * 2);
        Seek(hdSector);
        if (std::fread(table.data(), 1, table.size(), file.get()) != table.size())
            throw std::runtime_error("Truncated WBFS block table");
        blocks.resize(count);
        for (size_t i = 0; i < count; ++i) blocks[i] = U16(&table[0x100 + i * 2]);
    }
    // Reads disc bytes; blocks the image does not store read as zeros and count as holes.
    void Read(uint64_t offset, uint8_t* out, uint64_t size) {
        while (size) {
            const uint64_t index = offset / block, within = offset % block;
            const uint64_t chunk = std::min(size, block - within);
            const uint16_t stored = index < blocks.size() ? blocks[index] : 0;
            if (stored) {
                Seek(stored * block + within);
                if (std::fread(out, 1, chunk, file.get()) != chunk) throw std::runtime_error("Truncated WBFS image");
            } else {
                std::memset(out, 0, chunk);
                ++holes;
            }
            offset += chunk; out += chunk; size -= chunk;
        }
    }
    uint64_t holes = 0;

private:
    void Seek(uint64_t offset) {
        if (fseeko(file.get(), static_cast<off_t>(offset), SEEK_SET) != 0) throw std::runtime_error("WBFS seek failed");
    }
    File file;
    uint64_t block = 0;
    std::vector<uint16_t> blocks;
};

void Decrypt(const uint8_t* key, const uint8_t* iv, const uint8_t* in, uint8_t* out, size_t size) {
    CryptoPP::CBC_Mode<CryptoPP::AES>::Decryption aes(key, 16, iv);
    aes.ProcessData(out, in, size);
}

class Partition {
public:
    Partition(Wbfs& disc, uint64_t offset) : disc(disc) {
        uint8_t ticket[0x2C0];
        disc.Read(offset, ticket, sizeof(ticket));
        if (ticket[0x1F1] != 0) throw std::runtime_error("Only discs using the retail common key are supported");
        uint8_t iv[16] = {};
        std::memcpy(iv, ticket + 0x1DC, 8);
        Decrypt(kCommonKey, iv, ticket + 0x1BF, key, 16);
        base = offset + (uint64_t(U32(ticket + 0x2B8)) << 2);
    }
    // `offset` is in decrypted-data space.
    void Read(uint64_t offset, uint8_t* out, uint64_t size) {
        while (size) {
            const uint64_t index = offset / kClusterData, within = offset % kClusterData;
            if (index != cached) {
                disc.Read(base + index * kCluster, raw.data(), kCluster);
                Decrypt(key, raw.data() + 0x3D0, raw.data() + 0x400, plain.data(), kClusterData);
                cached = index;
            }
            const uint64_t chunk = std::min(size, kClusterData - within);
            std::memcpy(out, plain.data() + within, chunk);
            offset += chunk; out += chunk; size -= chunk;
        }
    }
    std::vector<uint8_t> Read(uint64_t offset, uint64_t size) {
        std::vector<uint8_t> bytes(size);
        Read(offset, bytes.data(), size);
        return bytes;
    }

private:
    Wbfs& disc;
    uint8_t key[16];
    uint64_t base = 0, cached = ~uint64_t(0);
    std::array<uint8_t, kCluster> raw{};
    std::array<uint8_t, kClusterData> plain{};
};

void WriteAll(const fs::path& path, const uint8_t* data, size_t size) {
    File file = Open(path, "wb");
    if (std::fwrite(data, 1, size, file.get()) != size || std::fflush(file.get()) != 0)
        throw std::runtime_error("Cannot write " + path.string() + " (is the storage full?)");
}

std::string ReadText(const fs::path& path) {
    std::FILE* raw = std::fopen(path.c_str(), "rb");
    if (!raw) return {};
    File file(raw);
    char buffer[256];
    const size_t n = std::fread(buffer, 1, sizeof(buffer), file.get());
    return std::string(buffer, n);
}

std::string MarkerText(const Identity& identity) {
    return std::string(kCacheFormat) + "\n" + identity.gameId + " rev" + std::to_string(identity.revision) + "\n";
}
}  // namespace

Identity ReadIdentity(const fs::path& wbfs) {
    Wbfs disc(wbfs);
    uint8_t header[8];
    disc.Read(0, header, sizeof(header));
    if (disc.holes) throw std::runtime_error("WBFS image has no disc header");
    Identity identity;
    identity.gameId.assign(reinterpret_cast<const char*>(header), 6);
    identity.revision = header[7];
    return identity;
}

void Extract(const fs::path& wbfs, const fs::path& destination, Progress& progress) {
    Wbfs disc(wbfs);
    uint8_t id[6];
    disc.Read(0, id, sizeof(id));
    uint64_t data = 0;
    for (uint32_t group = 0; group < 4 && !data; ++group) {
        uint8_t entry[8];
        disc.Read(0x40000 + group * 8, entry, 8);
        const uint32_t count = U32(entry);
        const uint64_t table = uint64_t(U32(entry + 4)) << 2;
        for (uint32_t i = 0; i < count && !data; ++i) {
            disc.Read(table + i * 8, entry, 8);
            if (U32(entry + 4) == 0) data = uint64_t(U32(entry)) << 2;  // type 0: game data
        }
    }
    if (!data) throw std::runtime_error("Disc image has no game data partition");
    Partition partition(disc, data);
    const auto boot = partition.Read(0, 0x440);
    static constexpr uint8_t kWiiMagic[4] = {0x5D, 0x1C, 0x9E, 0xA3};
    if (std::memcmp(boot.data(), id, 6) != 0 || std::memcmp(&boot[0x18], kWiiMagic, 4) != 0)
        throw std::runtime_error("Disc image decryption check failed");
    const uint64_t dolOffset = uint64_t(U32(&boot[0x420])) << 2;
    const uint64_t fstOffset = uint64_t(U32(&boot[0x424])) << 2, fstSize = uint64_t(U32(&boot[0x428])) << 2;

    const auto dolHeader = partition.Read(dolOffset, 0x100);
    uint64_t dolSize = 0;
    for (int i = 0; i < 18; ++i)
        if (const uint32_t size = U32(&dolHeader[0x90 + 4 * i]))
            dolSize = std::max<uint64_t>(dolSize, U32(&dolHeader[4 * i]) + uint64_t(size));
    const auto apploader = partition.Read(0x2440, 0x20);
    const uint64_t apploaderSize = 0x20 + uint64_t(U32(&apploader[0x14])) + U32(&apploader[0x18]);
    const auto fst = partition.Read(fstOffset, fstSize);
    if (fst.size() < 12) throw std::runtime_error("Disc file table is empty");
    const uint32_t entries = U32(&fst[8]);
    if (uint64_t(entries) * 12 > fst.size()) throw std::runtime_error("Disc file table is corrupt");
    const char* names = reinterpret_cast<const char*>(fst.data()) + entries * 12;
    const size_t namesSize = fst.size() - entries * 12;

    uint64_t total = 0x440 + 0x2000 + apploaderSize + dolSize + fstSize;
    for (uint32_t i = 1; i < entries; ++i)
        if (!fst[i * 12]) total += U32(&fst[i * 12 + 8]);
    progress.total = total;
    progress.done = 0;
    std::error_code ec;
    const auto space = fs::space(destination.parent_path(), ec);
    if (!ec && space.available < total + (64ull << 20))
        throw std::runtime_error("Not enough free storage to prepare the game data (" +
                                 std::to_string((total >> 20) + 64) + " MB needed)");

    // Gather every file first, then unpack them on several threads: on Android shared storage each
    // file's open/close costs tens of milliseconds, which overlaps well, and decryption spreads out.
    struct Job { fs::path path; uint64_t offset, size; };
    std::vector<Job> jobs;
    const fs::path sys = destination / "sys", files = destination / "files";
    fs::create_directories(sys);
    fs::create_directories(files);
    WriteAll(sys / "boot.bin", boot.data(), boot.size());
    WriteAll(sys / "fst.bin", fst.data(), fst.size());
    progress.done += boot.size() + fst.size();
    jobs.push_back({sys / "bi2.bin", 0x440, 0x2000});
    jobs.push_back({sys / "apploader.img", 0x2440, apploaderSize});
    jobs.push_back({sys / "main.dol", dolOffset, dolSize});

    auto name = [&](uint32_t i) {
        const uint32_t offset = U32(&fst[i * 12]) & 0xFFFFFF;
        if (offset >= namesSize) throw std::runtime_error("Disc file table name is corrupt");
        const char* begin = names + offset;
        const std::string text(begin, strnlen(begin, namesSize - offset));
        if (text.empty() || text == "." || text == ".." || text.find('/') != std::string::npos)
            throw std::runtime_error("Disc file table name is unsafe");
        return text;
    };
    std::function<void(uint32_t, uint32_t, const fs::path&)> walk = [&](uint32_t i, uint32_t end, const fs::path& dir) {
        while (i < end) {
            const uint32_t a = U32(&fst[i * 12 + 4]), b = U32(&fst[i * 12 + 8]);
            if (fst[i * 12]) {
                if (b <= i || b > end) throw std::runtime_error("Disc file table directory is corrupt");
                const fs::path sub = dir / name(i);
                fs::create_directories(sub);
                walk(i + 1, b, sub);
                i = b;
            } else {
                jobs.push_back({dir / name(i), uint64_t(a) << 2, b});
                ++i;
            }
        }
    };
    walk(1, entries, files);
    // Largest first, so one big file does not finish last on a single thread.
    std::stable_sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.size > b.size; });

    using Clock = std::chrono::steady_clock;
    auto nanos = [](Clock::time_point since) {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - since).count());
    };
    progress.files = uint32_t(jobs.size());
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> decryptNanos{0}, writeNanos{0};
    std::mutex failureMutex;
    std::exception_ptr failure;
    auto worker = [&] {
        try {
            Wbfs disc(wbfs);  // each thread reads through its own handle and cluster cache
            Partition partition(disc, data);
            std::vector<uint8_t> buffer(1 << 20);
            for (size_t index; (index = next++) < jobs.size();) {
                const Job& job = jobs[index];
                File file = Open(job.path, "wb");
                const uint64_t holesBefore = disc.holes;
                for (uint64_t at = 0; at < job.size;) {
                    if (progress.cancel) throw std::runtime_error("Preparing the game data was cancelled");
                    const uint64_t chunk = std::min<uint64_t>(buffer.size(), job.size - at);
                    auto start = Clock::now();
                    partition.Read(job.offset + at, buffer.data(), chunk);
                    decryptNanos += nanos(start);
                    start = Clock::now();
                    if (std::fwrite(buffer.data(), 1, chunk, file.get()) != chunk)
                        throw std::runtime_error("Cannot write " + job.path.string() + " (is the storage full?)");
                    writeNanos += nanos(start);
                    at += chunk;
                    progress.done += chunk;
                }
                if (std::fclose(file.release()) != 0) throw std::runtime_error("Cannot write " + job.path.string());
                ++progress.filesDone;
                if (disc.holes != holesBefore)
                    throw std::runtime_error("The disc image is incomplete: " + job.path.filename().string() + " is missing data");
            }
        } catch (...) {
            std::lock_guard lock(failureMutex);
            if (!failure) failure = std::current_exception();
            next = jobs.size();  // stop the other threads early
        }
    };
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < kThreads; ++i) threads.emplace_back(worker);
    for (auto& thread : threads) thread.join();
    progress.decryptNanos = decryptNanos;
    progress.writeNanos = writeNanos;
    if (failure) std::rethrow_exception(failure);
}

fs::path CachedRoot(const fs::path& parent, const Identity& identity) {
    const fs::path root = parent / "disc";
    std::error_code ec;
    if (ReadText(root / kMarker) == MarkerText(identity) && fs::is_regular_file(root / "sys/boot.bin", ec) &&
        fs::is_regular_file(root / "files/rel/relF.bin", ec))
        return root;
    return {};
}

fs::path ExtractToCache(const fs::path& wbfs, const fs::path& parent, const Identity& identity, Progress& progress) {
    const fs::path root = parent / "disc";
    fs::create_directories(parent);
    fs::remove_all(root);
    Extract(wbfs, root, progress);
    const std::string marker = MarkerText(identity);
    WriteAll(root / kMarker, reinterpret_cast<const uint8_t*>(marker.data()), marker.size());
    return root;
}
}  // namespace RuntimeFrontend::DiscImage
