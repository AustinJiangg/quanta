#!/bin/sh
# Cross-check Quanta's disassembler against the reference assembler's view.
#
# For each ELF passed as an argument, run it under `quanta --trace` and confirm
# that every instruction the tracer executes disassembles to the same mnemonic
# and operands that `objdump -d` prints at that address. objdump-only noise
# (symbol/comment annotations, 0x prefixes, whitespace) is normalised away, so
# the comparison is purely on the instruction text — which is exactly the M4
# "matches objdump -d" acceptance check.
#
# Needs the RISC-V cross-toolchain for objdump; skips cleanly without it.

OBJDUMP="${RVOBJDUMP:-riscv64-unknown-elf-objdump}"
QUANTA=./quanta
TMP="${TMPDIR:-/tmp}"

if ! command -v "$OBJDUMP" >/dev/null 2>&1; then
    echo "skip: $OBJDUMP not found (install the RISC-V cross-toolchain)"
    exit 0
fi

# Strip objdump-only decoration and collapse whitespace so two renderings of
# the same instruction compare equal.
norm() {
    sed -E -e 's/#.*$//' -e 's/<[^>]*>//g' -e 's/0x//g' \
           -e 's/[[:space:]]+/ /g' -e 's/ +$//'
}

rc=0
for elf in "$@"; do
    # objdump's "<pc> <mnemonic operands>" for every instruction in .text.
    "$OBJDUMP" -d "$elf" | grep -E '^[0-9a-f]{8}:' \
        | awk -F'\t' '{p=$1; sub(/:/,"",p); s=$3; for(i=4;i<=NF;i++)s=s" "$i; print p" "s}' \
        | norm | sort -u > "$TMP/qd_obj.txt"

    # quanta's tracer view: drop the "reg=val" and "->target" annotations,
    # leaving "<pc> <mnemonic operands>" to compare.
    "$QUANTA" --trace "$elf" 2>"$TMP/qd_trace.txt" >/dev/null
    sed -E -e 's/ +[a-z][a-z0-9]*=0x[0-9a-f]+//g' -e 's/ +->0x[0-9a-f]+//g' \
           -e 's/^([0-9a-f]+):  [0-9a-f]+  (.*)$/\1 \2/' "$TMP/qd_trace.txt" \
        | norm | sort -u > "$TMP/qd_got.txt"

    # Every instruction quanta executed must match objdump at the same address.
    comm -23 "$TMP/qd_got.txt" "$TMP/qd_obj.txt" > "$TMP/qd_diff.txt"
    if [ -s "$TMP/qd_diff.txt" ]; then
        echo "DIFF  $elf"
        while read -r pc rest; do
            exp=$(awk -v p="$pc" '$1==p{$1=""; sub(/^ /,""); print}' "$TMP/qd_obj.txt")
            printf '    %s  quanta:[%s]  objdump:[%s]\n' "$pc" "$rest" "$exp"
        done < "$TMP/qd_diff.txt"
        rc=1
    else
        echo "OK    $elf ($(wc -l < "$TMP/qd_got.txt" | tr -d ' ') instrs traced)"
    fi
done

if [ "$rc" -ne 0 ]; then echo "FAILED"; else echo "disassembly matches objdump"; fi
exit "$rc"
