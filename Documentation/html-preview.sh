#!/bin/sh
# Generate manual page preview as rendered by 'make html', removed some styling
# so there can be visual artifacts, usable for CI summary

if ! [ -f "$1" ]; then
	exit 0
fi

prefix=Documentation/
here=$(pwd)

if [ "$(basename \"$here\")" = 'Documentation' ]; then
	prefix=
fi

fn="$1"
bn=$(basename "$fn" .rst)
html=$(find "${prefix}"_build/html -name "$bn".'html')

if ! [ -f "$html" ]; then
	#echo "ERROR: cannot find html page '$html' from bn $bn fn $fn <br/>"
	exit 0
fi

cat << EOF
<details>
<summary>$fn</summary>

EOF

# Up to <div itemprop="articleBody">, before that there's left bar navigation

cat "$html" | sed -e 's/^\s\+//' | awk '
/^<div itemprop=.articleBody.>/ { doit=1; next; }
/^<\/body>/ { doit=0; next; }
doit==1 { print; }'

cat << EOF

</details>
EOF
