#include "zip_writer.hpp"

#include "dollyforge.h"

#include <stdexcept>

namespace dolly {
namespace {

void le16(std::vector<uint8_t> &o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v));
    o.push_back(static_cast<uint8_t>(v >> 8));
}

void le32(std::vector<uint8_t> &o, uint32_t v) {
    le16(o, v & 0xFFFF);
    le16(o, v >> 16);
}

constexpr uint32_t kDosDate = (0 << 9) | (1 << 5) | 1;  // 1980-01-01

}  // namespace

void ZipWriter::add(const std::string &name, const std::vector<uint8_t> &data) {
    if (name.empty() || name.size() > 0xFFFF) throw std::runtime_error("bad zip entry name: " + name);
    entries_.push_back({name, data, df_crc32(data.data(), data.size(), 0)});
}

void ZipWriter::add(const std::string &name, const std::string &text) {
    add(name, std::vector<uint8_t>(text.begin(), text.end()));
}

std::vector<uint8_t> ZipWriter::finish() const {
    std::vector<uint8_t> out, central;
    for (const Entry &e : entries_) {
        const auto offset = static_cast<uint32_t>(out.size());
        const auto size = static_cast<uint32_t>(e.data.size());
        le32(out, 0x04034b50);
        le16(out, 20);        // version needed
        le16(out, 0x0800);    // UTF-8 파일 이름
        le16(out, 0);         // stored
        le16(out, 0);         // time
        le16(out, kDosDate);
        le32(out, e.crc);
        le32(out, size);
        le32(out, size);
        le16(out, static_cast<uint32_t>(e.name.size()));
        le16(out, 0);
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.insert(out.end(), e.data.begin(), e.data.end());

        le32(central, 0x02014b50);
        le16(central, 20);
        le16(central, 20);
        le16(central, 0x0800);
        le16(central, 0);
        le16(central, 0);
        le16(central, kDosDate);
        le32(central, e.crc);
        le32(central, size);
        le32(central, size);
        le16(central, static_cast<uint32_t>(e.name.size()));
        le16(central, 0);
        le16(central, 0);
        le16(central, 0);
        le16(central, 0);
        le32(central, 0);
        le32(central, offset);
        central.insert(central.end(), e.name.begin(), e.name.end());
    }
    const auto central_offset = static_cast<uint32_t>(out.size());
    out.insert(out.end(), central.begin(), central.end());
    le32(out, 0x06054b50);
    le16(out, 0);
    le16(out, 0);
    le16(out, static_cast<uint32_t>(entries_.size()));
    le16(out, static_cast<uint32_t>(entries_.size()));
    le32(out, static_cast<uint32_t>(central.size()));
    le32(out, central_offset);
    le16(out, 0);
    return out;
}

}  // namespace dolly
