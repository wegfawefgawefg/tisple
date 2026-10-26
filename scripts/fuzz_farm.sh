#!/usr/bin/env bash
# differential fuzz farm: SHARDS processes, each running chunks of CHUNK random programs
# (compile, run on the simulator, compare with the interpreter) on its own seed range.
# logs one line per chunk to farm/shard-N.log; any chunk with mismatches keeps its full
# output in farm/fail-<seed>.log. stop with: kill -TERM -- -<pgid of this script>
set -u
cd "$(dirname "$0")/.."
SHARDS=${SHARDS:-12}
CHUNK=${CHUNK:-20000}
BASE=${BASE:-1000000000}
mkdir -p farm
shard() {
    local n=$1 seed=$((BASE + $1 * 1000000000))
    while true; do
        local out
        out=$(TISPLE_FUZZ=$CHUNK TISPLE_FUZZ_SEED=$seed ./build/tisple_tests 2>&1)
        local line
        line=$(printf '%s\n' "$out" | grep '^fuzz:' | tail -1)
        echo "$(date -u +%FT%TZ) seed $seed $line" >> "farm/shard-$n.log"
        if ! printf '%s\n' "$line" | grep -q ' 0 mismatches' ||
           ! printf '%s\n' "$out" | grep -q ', 0 failed'; then
            printf '%s\n' "$out" > "farm/fail-$seed.log"
        fi
        seed=$((seed + CHUNK))
    done
}
for n in $(seq 0 $((SHARDS - 1))); do
    shard "$n" &
done
wait
