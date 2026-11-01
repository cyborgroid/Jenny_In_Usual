# Dolly Forge

마인크래프트 캐릭터를 **메이크업(색·얼굴·의상)하고 개발(레시피 → 스킨 → 배포 팩)** 하는 툴킷입니다.
Project Haven — Jenny In Usual 의 모든 스킨은 이 도구로 만들어집니다.

![Studio](../05_Distribution/previews/Studio.png)

## 구조 — C 코어 하나를 여섯 언어가 공유

마인크래프트 자체가 Java(Java Edition) · C++(Bedrock) · Python(모딩/도구 생태계) 으로 나뉘어 있듯이,
Dolly Forge 도 각 언어가 맡는 자리가 있습니다. 단, **스킨 규격·레시피 해석·PNG·검증은 전부 C 코어 한 곳**에서
처리하므로 어떤 도구로 만들어도 결과가 픽셀 단위로 같습니다.

```
                      ┌─────────────────────────────┐
                      │  C   core/  libdollyforge    │  64x64 UV 레이아웃 · PNG 코덱(inflate 포함)
                      │      (C99, 의존성 없음)       │  · JSON · 레시피 실행 · 검증 · diff
                      └──────────────┬──────────────┘
        ┌──────────────┬─────────────┼──────────────┬────────────────┐
   static link     static link     ctypes        P/Invoke       같은 규격(64x64)
        │              │             │              │                │
 C++  cpp/        Rust rust/    Python python/  C#  csharp/     Java java/
 dolly-forge CLI  dolly-guard   바인딩·팔레트    Dolly Forge     DollyPreview
 + Bedrock        서명·검증·     추출·빌드        Studio (GUI)    3D 렌더러
 .mcpack 패키저   라이선스 린트   파이프라인       메이크업 편집기  (Java Edition 모델)
```

| 언어 | 위치 | 하는 일 |
|---|---|---|
| **C** | `core/` | `libdollyforge` — 스킨 데이터, 파트/면 UV 표, PNG 인코더·디코더, JSON 파서, 레시피 엔진, 검증기, 픽셀 diff |
| **C++** | `cpp/` | `dolly-forge` CLI (`forge` `validate` `info` `diff` `preview` `pack-bedrock`), Bedrock 스킨팩 ZIP 작성기 |
| **Java** | `java/` | `DollyPreview` — Java Edition 플레이어 모델(classic/slim, 2중 레이어) 정사영 3D 렌더, 라인업 이미지 |
| **Python** | `python/` | `dollyforge` ctypes 바인딩, `recipe` 포매터, 레퍼런스 팔레트 추출, 회귀 테스트, `build.py` |
| **C#** | `csharp/` | **Dolly Forge Studio** (Avalonia) — 팔레트 실시간 편집, 정면/후면/텍스처 미리보기, 변형 레시피 저장 |
| **Rust** | `rust/` | `dolly-guard` — 공식 스킨 SHA-256 서명, 무단 변형 감지(어느 파트가 몇 px 바뀌었는지), 레시피 메타/라이선스 린트 |

## 빌드

필요한 것: CMake 3.16+, C/C++ 컴파일러, JDK 17+, Python 3.10+ (Pillow), Rust (cargo), .NET 10 SDK.
모두 인터넷 없이 빌드됩니다 (Rust 는 외부 crate 없음, C# 는 Avalonia 11.3.22 NuGet 캐시 필요).

```bash
python3 DollyForge/build.py          # 전체: 테스트 → 스킨 → 서명 → 미리보기 → .mcpack → Studio
python3 DollyForge/build.py --no-studio
```

`build.py` 가 만드는 것:

| 결과물 | 위치 |
|---|---|
| 게임 적용 규격 스킨 PNG (64×64, slim) | `03_Sources/Minecraft_Skins/Jenny_*_Skin.png` |
| 레퍼런스 렌더에서 뽑은 팔레트 | `01_Concepts/reference_palette.json/.png` |
| 3D 미리보기 · 라인업 · Studio 스크린샷 | `05_Distribution/previews/` |
| Bedrock 스킨팩 | `05_Distribution/bedrock/JennyInUsual.mcpack` |
| 무결성 서명 | `05_Distribution/integrity.json` |

## 도구 사용법

```bash
B=DollyForge/build
$B/dolly-forge forge DollyForge/characters/jenny/jenny_cyber.dolly.json -o out.png --preview flat.png
$B/dolly-forge validate some_skin.png          # 64x64? base 레이어에 구멍? 레이아웃 밖 픽셀?
$B/dolly-forge diff official.png suspicious.png

G=DollyForge/rust/dolly-guard/target/release/dolly-guard
$G verify --manifest 05_Distribution/integrity.json suspicious.png
#  → OFFICIAL / MODIFIED — derived from Jenny_Cyber_Skin.png with 2 pixel(s) changed (head 1px, body 1px) / UNKNOWN

java -cp $B/java dollyforge.DollyPreview render skin.png preview.png --scale 12

dotnet DollyForge/csharp/DollyForge.Studio/bin/Release/net10.0/dolly-forge-studio.dll   # GUI
```

Python 에서:

```python
import sys; sys.path.insert(0, "DollyForge/python")
import dollyforge as df
skin = df.forge("DollyForge/characters/jenny/jenny_cyber.dolly.json")
print(skin.slim, skin.validate())          # True (0, [])
```

## 레시피 형식 (`*.dolly.json`)

스킨을 PNG 로 직접 그리는 대신 **레시피**로 기술합니다. 레시피는 사람이 읽고 리뷰할 수 있고(diff 가 의미 있음),
팔레트만 바꾼 "메이크업 변형"을 몇 줄로 만들 수 있습니다.

```jsonc
{
  "dollyforge": 1,
  "meta": { "id": "jenny_cyber", "title": { "en_US": "...", "ko_KR": "..." },
            "author": "...", "license": "CC-BY-NC-ND-4.0", "rating": "all-ages" },
  "extends": ["eyle_teal_base.dolly.json",  // 캐릭터의 얼굴·머리
              "../jenny/jenny_cyber.dolly.json"],  // + 의상 (파일 하나만 써도 됨)
  "model": "slim",                         // slim(3px 팔) | classic(4px 팔)
  "palette": {
    "// reference": "주석 키는 // 로 시작",
    "turtleneck": "#6E3B98",
    "turtleneck_dark": "turtleneck*0.8"    // 파생색: 원색을 바꾸면 따라 바뀜
  },
  "legend": { "T": "turtleneck", "t": "turtleneck_dark" },
  "ops": [
    { "op": "fill",   "part": "body", "faces": "all", "color": "coat" },
    { "op": "band",   "part": "body", "y": 7, "h": 1, "color": "belt" },
    { "op": "pixels", "part": "body", "face": "front", "rows": ["CCTTTTCC", "..."] },
    { "op": "mirror", "from": "arm_r", "to": "arm_l" }
  ]
}
```

| 항목 | 값 |
|---|---|
| `part` | `head` `body` `arm_r` `arm_l` `leg_r` `leg_l`, 묶음 `arms` `legs` `*`, 또는 배열 |
| `face(s)` | `top` `bottom` `right` `front` `left` `back`, 묶음 `sides` `all` |
| `layer` | `base` (기본, 반드시 불투명) · `overlay` (모자·재킷·소매·바지 2층) |
| 색 | `#RGB` `#RRGGBB` `#RRGGBBAA`, 팔레트 키, `clear`, `키*배율` |
| `fill` | 면 전체 칠하기 |
| `rect` | 면 안 사각형 (`x y w h`, 면 기준 좌표) |
| `band` | 측면 4개를 두르는 띠 (`y h`) — 벨트, 소맷부리, 부츠 |
| `pixels` | 픽셀 아트 (`rows` + `legend`, `.` 은 건너뜀, `flip` 으로 좌우 반전) |
| `mirror` | 팔/다리 한쪽을 반대쪽으로 좌우 반전 복사 (두 레이어 모두) |
| `shade` | 결정적 노이즈로 명도를 살짝 흔들어 천·머리결 표현 (`amount`, `seed`, `only`) |
| `note` | 주석 |

면 좌표는 그 면을 바깥에서 바라봤을 때 왼쪽 위가 (0,0) 입니다. `extends` 는 파일 하나 또는 배열이며,
부모들이 적힌 순서대로 먼저 실행되고(공통 조상은 한 번만), `palette`/`legend` 는 뒤에 오는 쪽이 덮어씁니다.
그래서 "캐릭터 얼굴 + 다른 캐릭터의 의상 + 색만 바꾼 자식" 이 몇 줄로 됩니다
(예: `characters/eyle_teal/eyle_teal_cyber.dolly.json`).

## 캐릭터 추가하기

1. `characters/<이름>/` 에 레시피를 만든다 (얼굴·머리는 `<이름>_base.dolly.json` 으로 분리, `"meta": {"abstract": true}`; 의상은 기존 레시피를 배열 `extends` 로 재사용).
2. Studio 에서 색을 다듬고 "변형 레시피 저장", 또는 레시피를 직접 편집한다.
3. `characters/jenny/collection.json` 에 넣고 (경로는 그 파일 기준) `python3 DollyForge/build.py`.
4. `dolly-guard lint` 가 메타·라이선스·연령 등급·게임 규격을 모두 통과해야 배포 팩에 들어간다.

## 명칭·공식성에 대해

- Dolly Forge 는 Mojang/Microsoft 의 공식 도구가 아니며, 마인크래프트 브랜드 가이드라인에 따라 공식처럼 보이게
  표기하지 않습니다. "공식"이 될 수 있는 범위는 **Project Haven 이 공식으로 쓰는 제작 도구**까지입니다.
  마인크래프트 안에서의 공식 유통은 Minecraft Marketplace 파트너 프로그램을 거쳐야 합니다.
- "Forge" 는 널리 쓰이는 모드 로더 *Minecraft Forge* 와 이름이 겹칩니다. 외부 공개 시 혼동이 없도록 설명을 붙이거나
  이름을 검토하는 것이 좋습니다.
- 이 폴더의 **코드** 라이선스는 아직 정하지 않았습니다. 스킨 에셋의 CC BY-NC-ND 4.0 은 코드에 맞지 않으므로
  (Creative Commons 도 소프트웨어에는 권장하지 않음) 코드는 별도 라이선스를 정해야 합니다.

NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR MICROSOFT.
