// libdollyforge 의 C++ RAII 래퍼
#pragma once

#include "dollyforge.h"
#include "dollyforge_json.h"

#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace dolly {

struct SkinDeleter { void operator()(df_skin *s) const { df_skin_free(s); } };
struct JsonDeleter { void operator()(df_json *j) const { df_json_free(j); } };
using Skin = std::unique_ptr<df_skin, SkinDeleter>;
using Json = std::unique_ptr<df_json, JsonDeleter>;

inline Skin load_skin(const std::string &path) {
    char err[256];
    Skin s(df_skin_load_png(path.c_str(), err, sizeof err));
    if (!s) throw std::runtime_error(err);
    return s;
}

inline Skin forge_file(const std::string &path) {
    char err[512];
    Skin s(df_forge_file(path.c_str(), err, sizeof err));
    if (!s) throw std::runtime_error(err);
    return s;
}

inline std::vector<uint8_t> read_bytes(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

inline void write_bytes(const std::string &path, const std::vector<uint8_t> &data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!f) throw std::runtime_error("cannot write " + path);
}

inline Json parse_json_file(const std::string &path) {
    auto bytes = read_bytes(path);
    char err[256];
    Json j(df_json_parse(reinterpret_cast<const char *>(bytes.data()), bytes.size(), err, sizeof err));
    if (!j) throw std::runtime_error(path + ": " + err);
    return j;
}

inline std::vector<uint8_t> encode_png(const uint8_t *rgba, int w, int h) {
    uint8_t *png = nullptr;
    size_t len = 0;
    if (!df_png_encode(rgba, w, h, &png, &len)) throw std::runtime_error("PNG encode failed");
    std::vector<uint8_t> out(png, png + len);
    df_free(png);
    return out;
}

// 검증 보고서와 오류 수
inline std::pair<int, std::string> validate(const df_skin *s) {
    std::string buf(8192, '\0');
    int errors = df_validate(s, buf.data(), buf.size());
    buf.resize(buf.find('\0'));
    return {errors, buf};
}

}  // namespace dolly
