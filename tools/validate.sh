#!/usr/bin/env bash
# usb-labs 轻量校验：Markdown 相对链接 + 代码围栏闭合
set -uo pipefail
cd "$(dirname "$0")/.."
rc=0; broken=0; unbalanced=0
while IFS= read -r f; do
  dir=$(dirname "$f")
  while IFS= read -r link; do
    [ -n "$link" ] || continue
    case "$link" in http*|\#*) continue ;; esac
    [ -e "$dir/$link" ] || { echo "断链: [$f] -> $link"; broken=$((broken+1)); }
  done < <(grep -oE '\]\([^)#][^)]*\)' "$f" 2>/dev/null | sed -E 's/^\]\(//; s/\)$//; s/#.*$//')
  n=$(grep -c '^```' "$f"); [ $((n % 2)) -ne 0 ] && { echo "围栏未闭合: $f"; unbalanced=$((unbalanced+1)); }
done < <(find . -path ./.git -prune -o -name '*.md' -type f -print)
echo "链接断链 $broken，围栏异常 $unbalanced"
[ "$broken" -eq 0 ] && [ "$unbalanced" -eq 0 ] && echo "✔ 通过" || rc=1
exit "$rc"
