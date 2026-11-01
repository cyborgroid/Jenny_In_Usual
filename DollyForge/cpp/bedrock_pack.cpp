#include "bedrock_pack.hpp"

#include "dolly.hpp"
#include "zip_writer.hpp"

#include <sstream>

namespace dolly {
namespace {

std::string quote(const std::string &s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        default: o += c;
        }
    }
    return o + "\"";
}

std::string str_field(const df_json *obj, const char *key, const std::string &where) {
    const char *s = df_json_str(df_json_get(obj, key));
    if (!s || !*s) throw std::runtime_error(where + ": missing string '" + key + "'");
    return s;
}

std::string dir_of(const std::string &path) {
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

// Bedrock 의 serialize_name / localization_name 은 영문·숫자·_ 만 안전하다
void check_ident(const std::string &id, const std::string &where) {
    for (char c : id)
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '_'))
            throw std::runtime_error(where + ": id '" + id + "' may only use A-Z, a-z, 0-9, _");
}

}  // namespace

std::string build_bedrock_pack(const std::string &pack_json_path, const std::string &out_path) {
    Json root = parse_json_file(pack_json_path);
    const df_json *pack = root.get();
    const std::string base = dir_of(pack_json_path);
    const std::string id = str_field(pack, "id", pack_json_path);
    check_ident(id, pack_json_path);
    const std::string header_uuid = str_field(pack, "header_uuid", pack_json_path);
    const std::string module_uuid = str_field(pack, "module_uuid", pack_json_path);

    int ver[3] = {1, 0, 0};
    const df_json *v = df_json_get(pack, "version");
    for (size_t i = 0; i < 3 && i < df_json_len(v); i++) ver[i] = static_cast<int>(df_json_num(df_json_at(v, i)));
    std::ostringstream version;
    version << "[" << ver[0] << ", " << ver[1] << ", " << ver[2] << "]";

    const df_json *names = df_json_get(pack, "name");
    if (df_json_kind(names) != DF_JSON_OBJECT || !df_json_get(names, "en_US"))
        throw std::runtime_error(pack_json_path + ": 'name' must be an object with at least en_US");

    ZipWriter zip;
    std::ostringstream skins_json, summary;
    skins_json << "{\n  \"skins\": [\n";
    const df_json *skins = df_json_get(pack, "skins");
    if (df_json_len(skins) == 0) throw std::runtime_error(pack_json_path + ": no skins");

    // 언어별 .lang 내용
    std::vector<std::pair<std::string, std::string>> langs;
    for (size_t i = 0; i < df_json_len(names); i++)
    {
        const std::string n = df_json_str(df_json_at(names, i)) ? df_json_str(df_json_at(names, i)) : "";
        // manifest 의 "pack.name" 과 스킨 선택 화면의 "skinpack.<id>" 둘 다 채운다
        langs.emplace_back(df_json_key_at(names, i), "pack.name=" + n + "\nskinpack." + id + "=" + n + "\n");
    }

    for (size_t i = 0; i < df_json_len(skins); i++) {
        const df_json *sk = df_json_at(skins, i);
        const std::string where = pack_json_path + ": skins[" + std::to_string(i) + "]";
        const std::string sid = str_field(sk, "id", where);
        check_ident(sid, where);
        const std::string texture = base + "/" + str_field(sk, "texture", where);
        Skin skin = load_skin(texture);
        auto [errors, report] = validate(skin.get());
        if (errors) throw std::runtime_error(texture + " failed validation:\n" + report);

        const bool slim = df_skin_model(skin.get()) == DF_MODEL_SLIM;
        zip.add(sid + ".png", read_bytes(texture));
        skins_json << (i ? ",\n" : "") << "    {\n"
                   << "      \"localization_name\": " << quote(sid) << ",\n"
                   << "      \"geometry\": " << quote(slim ? "geometry.humanoid.customSlim" : "geometry.humanoid.custom") << ",\n"
                   << "      \"texture\": " << quote(sid + ".png") << ",\n"
                   << "      \"type\": \"free\"\n    }";
        const df_json *sn = df_json_get(sk, "name");
        for (auto &[lang, text] : langs) {
            const char *n = df_json_str(df_json_get(sn, lang.c_str()));
            if (!n) n = df_json_str(df_json_get(sn, "en_US"));
            if (!n) throw std::runtime_error(where + ": 'name' needs en_US");
            text += "skin." + id + "." + sid + "=" + n + "\n";
        }
        summary << "  " << sid << " (" << (slim ? "slim" : "classic") << ") <- " << texture << "\n";
    }
    skins_json << "\n  ],\n  \"serialize_name\": " << quote(id) << ",\n  \"localization_name\": " << quote(id) << "\n}\n";

    const char *desc = df_json_str(df_json_get(pack, "description"));
    std::ostringstream manifest;
    manifest << "{\n  \"format_version\": 1,\n  \"header\": {\n"
             << "    \"name\": \"pack.name\",\n"
             << "    \"description\": " << quote(desc ? desc : "") << ",\n"
             << "    \"uuid\": " << quote(header_uuid) << ",\n"
             << "    \"version\": " << version.str() << "\n  },\n"
             << "  \"modules\": [\n    {\n      \"type\": \"skin_pack\",\n"
             << "      \"uuid\": " << quote(module_uuid) << ",\n"
             << "      \"version\": " << version.str() << "\n    }\n  ]\n}\n";

    std::ostringstream languages;
    languages << "[";
    for (size_t i = 0; i < langs.size(); i++) languages << (i ? ", " : "") << quote(langs[i].first);
    languages << "]\n";

    zip.add("manifest.json", manifest.str());
    zip.add("skins.json", skins_json.str());
    zip.add("texts/languages.json", languages.str());
    for (auto &[lang, text] : langs) zip.add("texts/" + lang + ".lang", text);
    write_bytes(out_path, zip.finish());

    return "Bedrock skin pack " + id + " -> " + out_path + "\n" + summary.str();
}

}  // namespace dolly
