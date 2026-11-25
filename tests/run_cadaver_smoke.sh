#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

CADAVER=${CADAVER:-cadaver}
PORT=${DAVBOX_PORT:-18080}
URL=${DAVBOX_URL:-http://localhost:${PORT}/}
COLLECTION="cadaver-smoke-$$"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

printf 'hello from cadaver\n' > "$work/hello.txt"

cat > "$work/commands.rc" <<EOF
pwd
ls
mkcol ${COLLECTION}
cd ${COLLECTION}
put ${work}/hello.txt hello.txt
ls
cat hello.txt
propnames hello.txt
propset hello.txt author mikael
propget hello.txt author
get hello.txt ${work}/downloaded.txt
copy hello.txt copy.txt
move copy.txt moved.txt
ls
lock hello.txt
discover hello.txt
unlock hello.txt
delete moved.txt
delete hello.txt
cd /
rmcol ${COLLECTION}
ls
quit
EOF

"$CADAVER" -r "$work/commands.rc" "$URL"
diff -u "$work/hello.txt" "$work/downloaded.txt"

echo "cadaver smoke test passed"
