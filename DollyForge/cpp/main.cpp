// dolly-forge: 레시피로 스킨을 만들고(forge), 검사하고(validate/diff), Bedrock 팩으로 묶는다.
#include "bedrock_pack.hpp"
#include "dolly.hpp"

#include <cstring>
#include <iostream>

namespace {

const char *kUsage =
    "Dolly Forge " "%s" " — Minecraft character makeup & development toolkit\n"
    "\n"
    "usage:\n"
    "  dolly-forge forge <recipe.dolly.json> -o <skin.png> [--preview <png>] [--scale N]\n"
    "  dolly-forge validate <skin.png>...\n"
    "  dolly-forge info <skin.png>\n"
    "  dolly-forge diff <official.png> <other.png>\n"
    "  dolly-forge preview <skin.png> -o <png> [--scale N]\n"
    "  dolly-forge pack-bedrock <pack.json> -o <out.mcpack>\n";

void usage() {
    std::fprintf(stderr, kUsage, df_version());
}

// 정면·후면 펼침 그림을 나란히 (사이 4px) 확대해서 PNG 로
void write_preview(const df_skin *skin, const std::string &path, int scale) {
    uint8_t front[16 * 32 * 4], back[16 * 32 * 4];
    df_render_flat(skin, 0, front);
    df_render_flat(skin, 1, back);
    const int w = (16 * 2 + 4) * scale, h = 32 * scale;
    std::vector<uint8_t> img(static_cast<size_t>(w) * h * 4, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int sx = x / scale, sy = y / scale;
            const uint8_t *src = sx < 16 ? &front[(sy * 16 + sx) * 4] : sx >= 20 ? &back[(sy * 16 + sx - 20) * 4] : nullptr;
            if (src) std::memcpy(&img[(static_cast<size_t>(y) * w + x) * 4], src, 4);
        }
    dolly::write_bytes(path, dolly::encode_png(img.data(), w, h));
}

int print_report(const std::string &name, const df_skin *skin) {
    auto [errors, report] = dolly::validate(skin);
    std::cout << name << ": " << (df_skin_model(skin) == DF_MODEL_SLIM ? "slim" : "classic") << ", "
              << (errors ? "INVALID" : "ok") << "\n";
    if (!report.empty()) std::cout << report;
    return errors;
}

struct Args {
    std::vector<std::string> pos;
    std::string out, preview;
    int scale = 8;
};

Args parse(int argc, char **argv, int from) {
    Args a;
    for (int i = from; i < argc; i++) {
        std::string s = argv[i];
        auto need = [&](const char *flag) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string(flag) + " needs a value");
            return argv[++i];
        };
        if (s == "-o" || s == "--out") a.out = need("-o");
        else if (s == "--preview") a.preview = need("--preview");
        else if (s == "--scale") {
            a.scale = std::stoi(need("--scale"));
            if (a.scale < 1 || a.scale > 64) throw std::runtime_error("--scale must be 1..64");
        } else a.pos.push_back(s);
    }
    return a;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    const std::string cmd = argv[1];
    try {
        Args a = parse(argc, argv, 2);
        if (cmd == "forge") {
            if (a.pos.size() != 1 || a.out.empty()) { usage(); return 2; }
            dolly::Skin skin = dolly::forge_file(a.pos[0]);
            int errors = print_report(a.pos[0], skin.get());
            char err[256];
            if (!df_skin_save_png(skin.get(), a.out.c_str(), err, sizeof err)) throw std::runtime_error(err);
            if (!a.preview.empty()) write_preview(skin.get(), a.preview, a.scale);
            std::cout << "  -> " << a.out << "\n";
            return errors ? 1 : 0;
        }
        if (cmd == "validate") {
            if (a.pos.empty()) { usage(); return 2; }
            int bad = 0;
            for (const auto &p : a.pos) bad += print_report(p, dolly::load_skin(p).get()) ? 1 : 0;
            return bad ? 1 : 0;
        }
        if (cmd == "info") {
            if (a.pos.size() != 1) { usage(); return 2; }
            dolly::Skin skin = dolly::load_skin(a.pos[0]);
            std::cout << a.pos[0] << "\n  model: " << (df_skin_model(skin.get()) == DF_MODEL_SLIM ? "slim (Alex, 3px arms)" : "classic (Steve, 4px arms)") << "\n";
            const uint8_t *px = df_skin_rgba_const(skin.get());
            for (int p = 0; p < DF_PART_COUNT; p++) {
                int painted[DF_LAYER_COUNT] = {0, 0}, total[DF_LAYER_COUNT] = {0, 0};
                for (int l = 0; l < DF_LAYER_COUNT; l++)
                    for (int f = 0; f < DF_FACE_COUNT; f++) {
                        int x, y, w, h;
                        df_face_rect(df_skin_model(skin.get()), (df_part)p, (df_layer)l, (df_face)f, &x, &y, &w, &h);
                        for (int j = y; j < y + h; j++)
                            for (int i = x; i < x + w; i++) {
                                total[l]++;
                                painted[l] += px[(j * DF_SKIN_W + i) * 4 + 3] ? 1 : 0;
                            }
                    }
                std::printf("  %-6s base %3d/%3d px, overlay %3d/%3d px\n", df_part_name((df_part)p), painted[0], total[0], painted[1], total[1]);
            }
            return print_report(a.pos[0], skin.get()) ? 1 : 0;
        }
        if (cmd == "diff") {
            if (a.pos.size() != 2) { usage(); return 2; }
            dolly::Skin x = dolly::load_skin(a.pos[0]), y = dolly::load_skin(a.pos[1]);
            int diff[DF_PART_COUNT];
            int total = df_diff(x.get(), y.get(), diff);
            if (!total) { std::cout << "identical pixels\n"; return 0; }
            std::cout << total << " pixel(s) differ\n";
            if (df_skin_model(x.get()) != df_skin_model(y.get())) std::cout << "  model differs (slim/classic)\n";
            for (int p = 0; p < DF_PART_COUNT; p++)
                if (diff[p]) std::cout << "  " << df_part_name((df_part)p) << ": " << diff[p] << "\n";
            return 1;
        }
        if (cmd == "preview") {
            if (a.pos.size() != 1 || a.out.empty()) { usage(); return 2; }
            write_preview(dolly::load_skin(a.pos[0]).get(), a.out, a.scale);
            std::cout << a.out << "\n";
            return 0;
        }
        if (cmd == "pack-bedrock") {
            if (a.pos.size() != 1 || a.out.empty()) { usage(); return 2; }
            std::cout << dolly::build_bedrock_pack(a.pos[0], a.out);
            return 0;
        }
        if (cmd == "--version" || cmd == "version") { std::cout << "dolly-forge " << df_version() << "\n"; return 0; }
        usage();
        return 2;
    } catch (const std::exception &e) {
        std::cerr << "dolly-forge: " << e.what() << "\n";
        return 1;
    }
}
