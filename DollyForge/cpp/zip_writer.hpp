// 무압축(stored) ZIP 작성기. .mcpack 은 이름만 바뀐 ZIP 이다.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dolly {

class ZipWriter {
public:
    void add(const std::string &name, const std::vector<uint8_t> &data);
    void add(const std::string &name, const std::string &text);
    // 같은 입력이면 바이트 단위로 같은 파일이 나오도록 시각은 1980-01-01 로 고정
    std::vector<uint8_t> finish() const;

private:
    struct Entry {
        std::string name;
        std::vector<uint8_t> data;
        uint32_t crc;
    };
    std::vector<Entry> entries_;
};

}  // namespace dolly
