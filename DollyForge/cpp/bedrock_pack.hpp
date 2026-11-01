// Bedrock Edition(C++ 엔진) 스킨팩(.mcpack) 빌더
#pragma once

#include <string>

namespace dolly {

// pack.json 을 읽어 스킨을 검증하고 .mcpack 을 쓴다. 실패 시 std::runtime_error.
// 반환값: 사람이 읽을 요약
std::string build_bedrock_pack(const std::string &pack_json_path, const std::string &out_path);

}  // namespace dolly
