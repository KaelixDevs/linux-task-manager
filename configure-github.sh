#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <github-username>"
  exit 1
fi

USER_NAME="$1"
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

while IFS= read -r -d '' file; do
  sed -i "s/YOUR_GITHUB_USERNAME/$USER_NAME/g" "$file"
done < <(grep -rlZ --exclude-dir=.git -- 'YOUR_GITHUB_USERNAME' "$ROOT" || true)

echo "GitHub URLs set to: https://github.com/$USER_NAME/linux-task-manager"
