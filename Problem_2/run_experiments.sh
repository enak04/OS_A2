#!/bin/bash
# Runs Problem 2 end to end and writes results.txt
# Usage: sudo ./run_experiments.sh
# Optional: PRIO=20 sudo -E ./run_experiments.sh   (priority range 10-50)

PRIO=${PRIO:-10}
OUT=${OUT:-results.txt}
CFLAGS=${CFLAGS:--O2 -Wall -I.}
QUANTA=(1 10 50 150 300 500)
KNOB=/proc/sys/kernel/sched_rr_timeslice_ms
DASHES="--------------------------------------------------"

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run with sudo (needs root for chrt and the kernel parameter)." >&2
    exit 1
fi

ORIG=$(cat "$KNOB")
trap 'echo "$ORIG" > "$KNOB"' EXIT   # restore the original timeslice when done

gcc $CFLAGS -o program_a program_a.c || exit 1
gcc $CFLAGS -o program_b program_b.c || exit 1

: > "$OUT"

run_program() {
    local label=$1 bin=$2

    echo "$label" >> "$OUT"
    echo "$DASHES" >> "$OUT"

    # Round-robin runs, one per time quantum
    for q in "${QUANTA[@]}"; do
        echo "$q" > "$KNOB"
        echo "[$bin] SCHED_RR, quantum ${q}ms ..." >&2
        taskset -c 0 chrt -r "$PRIO" "./$bin" >> "$OUT"
        echo >> "$OUT"
    done

    # Single FIFO run, capped at 30 s
    echo "[$bin] SCHED_FIFO ..." >&2
    timeout 30 taskset -c 0 chrt -f "$PRIO" "./$bin" >> "$OUT"
    if [ $? -eq 124 ]; then
        pkill -KILL -x "$bin"      # timeout only kills the parent; remove the children too
        echo "Scheduling Policy: SCHED_FIFO" >> "$OUT"
        echo "  [run exceeded the 30 s timeout]" >> "$OUT"
    fi
    echo >> "$OUT"
}

run_program "Program-A (CPU-Bound)" program_a
run_program "Program-B (I/O-Bound)" program_b

echo "Done. Results written to $OUT" >&2
