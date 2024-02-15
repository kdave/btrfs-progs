#!/bin/sh
#

for fn in *.c; do

	cat << EOF
<details>
<summary>$fn</summary>

\`\`\`
EOF
	cat "$fn"
	cat << EOF
\`\`\`

</details>
EOF
done
