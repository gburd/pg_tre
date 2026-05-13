#!/bin/bash
export TMPDIR=~/tmp/pg_tre_test
export PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config
export PG_REGRESS=/home/gburd/.pgrx/18.3/pgrx-install/lib/postgresql/pgxs/src/test/regress/pg_regress
export PERL5LIB=/home/gburd/.pgrx/18.3/src/test/perl
export PATH=$PATH:~/.pgrx/18.3/pgrx-install/bin

crashes=0
exits_4=0
exits_0=0

for i in {1..10}; do
  echo "=== Run $i/10 ==="
  
  # Cleanup
  pkill -9 postgres 2>/dev/null || true
  sleep 1
  [ -d tmp_check ] && mv tmp_check ~/tmp/pg_tre_test/old_tmp_check-$(date +%s)-$i 2>/dev/null || true
  
  # Run test
  timeout 90 nix-shell -p '(perl.withPackages (p: [ p.IPCRun ]))' --run "perl tap/concurrency.pl" 2>&1 | tail -5
  exit_code=${PIPESTATUS[0]}
  
  if [ $exit_code -eq 0 ]; then
    exits_0=$((exits_0 + 1))
    echo "✓ PASS (exit 0)"
  elif [ $exit_code -eq 4 ]; then
    exits_4=$((exits_4 + 1))
    echo "✓ PASS (exit 4 - test failure but no crash)"
  elif [ $exit_code -eq 29 ] || [ $exit_code -eq 139 ]; then
    crashes=$((crashes + 1))
    echo "✗ CRASH (exit $exit_code)"
  else
    echo "? exit $exit_code"
  fi
  echo
done

echo "================================"
echo "Results:"
echo "  Clean exits (0): $exits_0"
echo "  Test fails (4): $exits_4"
echo "  Crashes: $crashes"
echo "================================"
