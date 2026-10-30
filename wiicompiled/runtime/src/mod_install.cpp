#include "mod_install.h"

#include "runtime_config.h"
#include "runtime_log.h"

#include <cryptopp/filters.h>
#include <cryptopp/zinflate.h>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <cstdint>
#include <fstream>
#include <iterator>

namespace ModInstall {
namespace {
namespace fs = std::filesystem;

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

// The folder holding files/ or textures/: `dir` itself, or the first one found up to three levels down.
fs::path FindRoot(const fs::path& dir, int depth = 3) {
    std::error_code ec;
    if (fs::is_directory(dir / "files", ec) || fs::is_directory(dir / "textures", ec)) return dir;
    if (depth == 0) return {};
    std::vector<fs::path> subs;
    for (const auto& entry : fs::directory_iterator(dir, ec))
        if (entry.is_directory(ec)) subs.push_back(entry.path());
    std::sort(subs.begin(), subs.end());
    for (const fs::path& sub : subs)
        if (fs::path found = FindRoot(sub, depth - 1); !found.empty()) return found;
    return {};
}

// A folder name safe on every system.
std::string SafeName(std::string name) {
    for (char& c : name)
        if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 32) c = '_';
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    return name.empty() || name == ".." ? "mod" : name;
}
} // namespace

fs::path Folder() { return RuntimeConfigFile::ApplicationDataDirectory() / "mods"; }

bool ExtractZip(const fs::path& zip, const fs::path& dest, std::string& error) {
    std::ifstream in(zip, std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
    // End of central directory: the last "PK\5\6" within the final 64 KB + 22 bytes.
    size_t eocd = std::string::npos;
    for (size_t i = data.size() >= 22 ? data.size() - 22 : 0; data.size() >= 22; --i) {
        if (Le32(&data[i]) == 0x06054B50u) {
            eocd = i;
            break;
        }
        if (i == 0 || data.size() - i > 0x10000 + 22) break;
    }
    if (eocd == std::string::npos) {
        error = "not a zip file";
        return false;
    }
    const uint16_t count = Le16(&data[eocd + 10]);
    size_t at = Le32(&data[eocd + 16]);
    const fs::path root = fs::absolute(dest).lexically_normal();
    try {
        for (uint16_t n = 0; n < count; ++n) {
            if (at + 46 > data.size() || Le32(&data[at]) != 0x02014B50u) throw std::runtime_error("damaged zip");
            const uint16_t method = Le16(&data[at + 10]);
            const uint32_t packed = Le32(&data[at + 20]), size = Le32(&data[at + 24]);
            const uint16_t nameLen = Le16(&data[at + 28]), extraLen = Le16(&data[at + 30]), commentLen = Le16(&data[at + 32]);
            const uint32_t local = Le32(&data[at + 42]);
            if (at + 46 + nameLen > data.size()) throw std::runtime_error("damaged zip");
            std::string name(reinterpret_cast<const char*>(&data[at + 46]), nameLen);
            at += 46 + size_t(nameLen) + extraLen + commentLen;
            std::replace(name.begin(), name.end(), '\\', '/');
            if (name.empty()) continue;
            const fs::path target = (root / RuntimeConfigFile::PathFromUtf8(name)).lexically_normal();
            const auto rel = target.lexically_relative(root);
            if (rel.empty() || *rel.begin() == ".." || fs::path(RuntimeConfigFile::PathFromUtf8(name)).has_root_path())
                throw std::runtime_error("the zip holds a file outside its own folder: " + name);
            if (name.back() == '/') {
                fs::create_directories(target);
                continue;
            }
            if (packed == 0xFFFFFFFFu || local + 30 > data.size()) throw std::runtime_error("zip64 archives are not supported");
            const size_t start = local + 30 + Le16(&data[local + 26]) + Le16(&data[local + 28]);
            if (start + packed > data.size()) throw std::runtime_error("damaged zip");
            std::string out;
            if (method == 0) {
                out.assign(reinterpret_cast<const char*>(&data[start]), packed);
            } else if (method == 8) {
                CryptoPP::Inflator inflator(new CryptoPP::StringSink(out));
                inflator.Put(&data[start], packed);
                inflator.MessageEnd();
            } else {
                throw std::runtime_error("unsupported compression in " + name);
            }
            if (out.size() != size) throw std::runtime_error("damaged zip entry " + name);
            fs::create_directories(target.parent_path());
            std::ofstream file(target, std::ios::binary | std::ios::trunc);
            file.write(out.data(), std::streamsize(out.size()));
            if (!file) throw std::runtime_error("can't write " + RuntimeConfigFile::PathToUtf8(target));
        }
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

bool SelfCheck() {
    // A stored zip "My Mod/files/a.txt" installs as mods-like folder content; "../evil.txt" is refused.
    auto zipOf = [](const std::string& name, const std::string& body) {
        std::string z, cd;
        auto u16 = [](std::string& o, uint16_t v) { o += char(v & 255); o += char(v >> 8); };
        auto u32 = [&](std::string& o, uint32_t v) { u16(o, uint16_t(v & 0xFFFF)); u16(o, uint16_t(v >> 16)); };
        u32(z, 0x04034B50u); u16(z, 20); u16(z, 0); u16(z, 0); u16(z, 0); u16(z, 0); u32(z, 0);
        u32(z, uint32_t(body.size())); u32(z, uint32_t(body.size())); u16(z, uint16_t(name.size())); u16(z, 0);
        z += name + body;
        u32(cd, 0x02014B50u); u16(cd, 20); u16(cd, 20); u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0); u32(cd, 0);
        u32(cd, uint32_t(body.size())); u32(cd, uint32_t(body.size())); u16(cd, uint16_t(name.size()));
        u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0); u32(cd, 0); u32(cd, 0);
        cd += name;
        const uint32_t cdAt = uint32_t(z.size());
        z += cd;
        u32(z, 0x06054B50u); u16(z, 0); u16(z, 0); u16(z, 1); u16(z, 1); u32(z, uint32_t(cd.size())); u32(z, cdAt); u16(z, 0);
        return z;
    };
    const fs::path work = fs::temp_directory_path() / "nspb-modcheck";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    std::ofstream(work / "good.zip", std::ios::binary) << zipOf("My Mod/files/a.txt", "hello");
    std::ofstream(work / "bad.zip", std::ios::binary) << zipOf("../evil.txt", "x");
    std::string error;
    const bool good = ExtractZip(work / "good.zip", work / "out", error) && FindRoot(work / "out") == work / "out" / "My Mod" &&
                      fs::file_size(work / "out" / "My Mod" / "files" / "a.txt", ec) == 5;
    const bool bad = !ExtractZip(work / "bad.zip", work / "out2", error) && !fs::exists(work / "evil.txt", ec);
    fs::remove_all(work, ec);
    return good && bad;
}

void InstallDroppedZips() {
    std::error_code ec;
    const fs::path mods = Folder();
    std::vector<fs::path> zips;
    for (const auto& entry : fs::directory_iterator(mods, ec)) {
        std::string ext = RuntimeConfigFile::PathToUtf8(entry.path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (entry.is_regular_file(ec) && ext == ".zip") zips.push_back(entry.path());
    }
    for (const fs::path& zip : zips) {
        const fs::path temp = mods / ".installing";
        fs::remove_all(temp, ec);
        std::string error;
        fs::path modRoot;
        if (ExtractZip(zip, temp, error)) {
            modRoot = FindRoot(temp);
            if (modRoot.empty()) error = "no files/ or textures/ folder inside";
        }
        if (modRoot.empty()) {
            RT_LOGF(RT_TAG_RUNTIME, "mods: %s not installed (%s)\n", RuntimeConfigFile::PathToUtf8(zip.filename()).c_str(), error.c_str());
            fs::rename(zip, fs::path(zip).concat(".not-a-mod"), ec);  // not retried every start
            fs::remove_all(temp, ec);
            continue;
        }
        const std::string name = SafeName(RuntimeConfigFile::PathToUtf8(modRoot == temp ? zip.stem() : modRoot.filename()));
        const fs::path target = mods / RuntimeConfigFile::PathFromUtf8(name);
        fs::remove_all(target, ec);  // a newer zip of the same mod replaces it
        fs::rename(modRoot, target, ec);
        fs::remove_all(temp, ec);
        if (ec) {
            RT_LOGF(RT_TAG_RUNTIME, "mods: %s not installed (%s)\n", name.c_str(), ec.message().c_str());
            continue;
        }
        fs::remove(zip, ec);
        RT_LOGF(RT_TAG_RUNTIME, "mods: installed %s\n", name.c_str());
    }
}

std::vector<Mod> List() {
    std::vector<Mod> mods;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(Folder(), ec)) {
        const std::string name = RuntimeConfigFile::PathToUtf8(entry.path().filename());
        if (entry.is_directory(ec) && !name.empty() && name[0] != '.')
            mods.push_back({name, !fs::exists(entry.path() / "disabled", ec)});
    }
    std::sort(mods.begin(), mods.end(), [](const Mod& a, const Mod& b) { return a.name < b.name; });
    return mods;
}

void SetEnabled(const std::string& name, bool enabled) {
    const fs::path marker = Folder() / RuntimeConfigFile::PathFromUtf8(name) / "disabled";
    std::error_code ec;
    if (enabled) fs::remove(marker, ec);
    else std::ofstream(marker).put('\n');
}
} // namespace ModInstall
