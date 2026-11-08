#!/bin/bash
# Prints the recompiled function containing a guest address: devtools/func_of.sh 826DA7C8
a=$(echo "$1" | tr a-f A-F)
f=$(grep -l "// $a " generated/*.cpp | head -1)
awk -v A="// $a " '/^PPC_FUNC_IMPL/{name=$0} index($0,A){print FILENAME": "name; exit}' "$f"
