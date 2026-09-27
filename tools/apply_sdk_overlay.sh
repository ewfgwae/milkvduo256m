#!/bin/bash
# 把本仓库的改动覆盖进 duo-sdk-v2。
# 用法: tools/apply_sdk_overlay.sh [SDK路径]     默认 ~/duo-sdk-v2
set -euo pipefail

SDK="${1:-$HOME/duo-sdk-v2}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OVL="$HERE/src/sdk_overlay"

[ -d "$SDK" ] || { echo "找不到 SDK: $SDK" >&2; exit 1; }
[ -d "$OVL" ] || { echo "找不到 overlay: $OVL" >&2; exit 1; }

echo "SDK  : $SDK"
echo "覆盖 : $OVL"

# 先看一眼 SDK 基线是否一致（改动是基于 ad920f839 的）
if [ -d "$SDK/.git" ]; then
	base="$(git -C "$SDK" rev-parse --short HEAD 2>/dev/null || echo '?')"
	echo "SDK 当前提交: $base  (本 overlay 基于 ad920f839)"
fi

echo "--- 备份将被覆盖的文件到 $SDK/.overlay_backup ---"
stamp="$(date +%Y%m%d_%H%M%S)"
( cd "$OVL" && find . -type f ) | while read -r f; do
	if [ -f "$SDK/$f" ]; then
		mkdir -p "$SDK/.overlay_backup/$stamp/$(dirname "$f")"
		cp -a "$SDK/$f" "$SDK/.overlay_backup/$stamp/$f"
	fi
done

echo "--- 覆盖 ---"
( cd "$OVL" && tar cf - . ) | tar xf - -C "$SDK"

echo "--- 完成，覆盖的文件清单 ---"
( cd "$OVL" && find . -type f | sed 's|^\./||' | sort )
echo
echo "下一步: tools/build_small_core.sh $SDK"
