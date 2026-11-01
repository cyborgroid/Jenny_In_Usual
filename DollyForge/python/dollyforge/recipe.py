"""레시피(.dolly.json) 읽기/쓰기. 픽셀 아트 rows 가 한 줄에 한 행씩 보이도록 정렬해서 쓴다."""
import json

_ROW_KEYS = ("rows",)


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def _scalar(v):
    return json.dumps(v, ensure_ascii=False)


def _op(op, indent):
    pad = " " * indent
    parts = []
    rows = None
    for k, v in op.items():
        if k in _ROW_KEYS and isinstance(v, list):
            rows = (k, v)
            continue
        parts.append(f"{_scalar(k)}: {_scalar(v)}")
    if rows is None:
        return pad + "{ " + ", ".join(parts) + " }"
    k, v = rows
    inner = (",\n").join(f"{pad}  {_scalar(r)}" for r in v)
    head = ", ".join(parts + [f"{_scalar(k)}: ["])
    return f"{pad}{{ {head}\n{inner}\n{pad}] }}"


def _flat_object(obj, indent, per_line=1):
    pad = " " * indent
    items = [f"{_scalar(k)}: {_scalar(v)}" for k, v in obj.items()]
    if not items:
        return "{}"
    lines = [", ".join(items[i:i + per_line]) for i in range(0, len(items), per_line)]
    return "{\n" + ",\n".join(pad + "  " + l for l in lines) + "\n" + pad + "}"


def dumps(recipe):
    out = ["{"]
    keys = list(recipe.keys())
    for i, k in enumerate(keys):
        v = recipe[k]
        comma = "," if i < len(keys) - 1 else ""
        if k == "ops":
            body = ",\n".join(_op(op, 4) for op in v)
            out.append(f'  "ops": [\n{body}\n  ]{comma}')
        elif k in ("palette", "legend") and isinstance(v, dict):
            out.append(f"  {_scalar(k)}: {_flat_object(v, 2, 1 if k == 'palette' else 4)}{comma}")
        elif isinstance(v, dict):
            out.append(f"  {_scalar(k)}: " + json.dumps(v, ensure_ascii=False, indent=2).replace("\n", "\n  ") + comma)
        else:
            out.append(f"  {_scalar(k)}: {_scalar(v)}{comma}")
    out.append("}")
    return "\n".join(out) + "\n"


def save(recipe, path):
    with open(path, "w", encoding="utf-8") as f:
        f.write(dumps(recipe))
