#!/bin/sh
# Run every tests/run_*_host_test.sh. Print one PASS or FAIL line for each test.
# Exit non-zero if one test fails.
#
# CC and HOST_TEST_CFLAGS go through to each script, for example:
#   CC=clang HOST_TEST_CFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -g" \
#     sh tests/run_host_tests.sh
set -u

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
pass=0
fail=0
failed=""

for t in "$root"/tests/run_*_host_test.sh; do
    name="$(basename -- "$t" .sh)"
    name="${name#run_}"
    echo "==> $name"
    if sh "$t"; then
        pass=$((pass + 1))
        echo "PASS $name"
    else
        fail=$((fail + 1))
        failed="$failed $name"
        echo "FAIL $name"
    fi
done

echo "host tests: $pass passed, $fail failed"
if [ "$fail" -ne 0 ]; then
    echo "failed:$failed"
    exit 1
fi
