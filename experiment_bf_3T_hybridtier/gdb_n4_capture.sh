#!/usr/bin/env bash
# =============================================================================
# N4 capture: catch EnsureFailed in tpcc_two_level, dump the failing
# LeanStoreAdapter::update1 frame's folded_key + dt_id so we can decode
# which table + row was missing.
#
# Notes:
# - JumpMU uses setjmp/longjmp (NOT C++ throws), so `catch throw` here only
#   fires on real exceptions (EnsureFailed / UnReachable / TODO / Generic).
# - N4 is intermittent. Pass MAX_RETRY=N as env var to retry until it fires.
# - Run with the same build/ binary used by run_medium_benchmark.sh.
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"
BINARY="$BUILD_DIR/tpcc_compare_test"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_n4_gdb"
CXL_DAX_DEVICE="/dev/dax0.1"

MAX_RETRY="${MAX_RETRY:-3}"

TS=$(date +%Y%m%d_%H%M%S)
OUT_DIR="$SCRIPT_DIR/gdb_n4_${TS}"
mkdir -p "$OUT_DIR"

if [[ ! -x "$BINARY" ]]; then
   echo "[ERR] binary not found: $BINARY" >&2
   exit 1
fi

# Quick sanity: has debug info?
if ! readelf --debug-dump=info "$BINARY" 2>/dev/null | head -3 | grep -q "DWARF"; then
   echo "[WARN] $BINARY may lack DWARF debug info; gdb dumps will be sparse"
fi

GDB_SCRIPT="$OUT_DIR/cmds.gdb"
cat > "$GDB_SCRIPT" <<'GDBEOF'
set pagination off
set print pretty on
set print object on
set print frame-arguments all
set logging file __OUT_DIR__/gdb_capture.log
set logging overwrite on
set logging redirect on
set logging on

# Only catch C++ throws (jumpmu uses setjmp, never throws)
catch throw
commands
  silent
  printf "\n================ throw caught ================\n"
  shell date '+wall %Y-%m-%d %H:%M:%S.%N'
  printf "\n--- thread ---\n"
  thread
  printf "\n--- backtrace top 30 ---\n"
  bt 30
  printf "\n--- python walker: find LeanStoreAdapter::update1 frame ---\n"
python
import gdb
try:
    f = gdb.newest_frame()
    depth = 0
    found = False
    while f is not None and depth < 40:
        sym = f.function()
        name = sym.print_name if sym else '<no-sym>'
        if name and 'update1' in name and 'LeanStoreAdapter' in name:
            print('[N4-walker] match at depth', depth, ':', name)
            f.select()
            # Dump folded_key_len and folded_key bytes
            try:
                k_len = int(gdb.parse_and_eval('folded_key_len'))
                print('[N4-walker] folded_key_len =', k_len)
                bytes_hex = ''
                for i in range(min(k_len, 64)):
                    bytes_hex += '%02x ' % (int(gdb.parse_and_eval('folded_key[%d]' % i)) & 0xff)
                print('[N4-walker] folded_key =', bytes_hex)
            except Exception as e:
                print('[N4-walker] err dumping key:', repr(e))
            # Dump btree dt_id (each table has unique dt_id)
            try:
                dt_id = int(gdb.parse_and_eval('this->btree->dt_id'))
                print('[N4-walker] btree->dt_id =', dt_id)
            except Exception as e:
                print('[N4-walker] err dumping dt_id:', repr(e))
            # Dump *this type to get Record template arg (table type)
            try:
                t = gdb.parse_and_eval('*this').type
                print('[N4-walker] *this type =', str(t))
            except Exception as e:
                print('[N4-walker] err dumping this type:', repr(e))
            # Dump res (should be NOT_FOUND=...)
            try:
                r = gdb.parse_and_eval('res')
                print('[N4-walker] res =', str(r))
            except Exception as e:
                print('[N4-walker] err dumping res:', repr(e))
            found = True
            break
        f = f.older()
        depth += 1
    if not found:
        print('[N4-walker] LeanStoreAdapter::update1 frame NOT found in top 40')
        # As fallback, dump info locals from current frame
        gdb.execute('info locals', to_string=False)
except Exception as e:
    print('[N4-walker] outer exception:', repr(e))
end
  printf "\n--- info locals (current frame) ---\n"
  info locals
  printf "\n================ end capture ================\n"
  continue
end

# When the process is about to terminate (uncaught EnsureFailed → terminate),
# we want to bail rather than waiting forever.
catch signal SIGABRT
commands
  silent
  printf "\n================ SIGABRT (terminate) ================\n"
  bt 20
  printf "\n================ exiting gdb ================\n"
  kill
  quit 0
end

run \
    --tpcc_warehouse_count=10 --test_warmup_seconds=20 --test_measure_seconds=40 \
    --test_load_data=true --worker_threads=8 --vi=true --wal=true --trunc=true \
    --ssd_path=__SSD_PATH__ \
    --test_admission_mode=two_level \
    --cxl_tiering_enabled=true --cxl_gib=2.0 \
    --cxl_dax_device_path=__CXL_DAX_DEVICE__ \
    --pp_threads=1 --cxl_pp_threads=1 \
    --two_level_admission_threads=2 \
    --delay_admission_recordcache_threads_start=true \
    --dram_buffer_pool_gib=0.125 \
    --dram_recordcache_gib=0.375 \
    --forward_epoch_thread=1 \
    --sieve_eviction_thread=1 \
    --record_cache_promote_thread=4

set logging off
quit
GDBEOF

# Substitute placeholders
sed -i "s|__OUT_DIR__|$OUT_DIR|g; s|__SSD_PATH__|$SSD_PATH|g; s|__CXL_DAX_DEVICE__|$CXL_DAX_DEVICE|g" "$GDB_SCRIPT"

attempt=1
while (( attempt <= MAX_RETRY )); do
   echo ""
   echo "============================================================"
   echo "[INFO] attempt $attempt / $MAX_RETRY  ($(date '+%H:%M:%S'))"
   echo "[INFO] binary    : $BINARY"
   echo "[INFO] out dir   : $OUT_DIR"
   echo "[INFO] gdb cmds  : $GDB_SCRIPT"
   echo "[INFO] capture   : $OUT_DIR/gdb_capture.log"
   echo "[INFO] stdout    : $OUT_DIR/stdout_attempt${attempt}.log"
   echo "============================================================"

   rm -f "$SSD_PATH"

   gdb -batch -x "$GDB_SCRIPT" "$BINARY" 2>&1 | tee "$OUT_DIR/stdout_attempt${attempt}.log"

   if grep -q "N4-walker" "$OUT_DIR/gdb_capture.log" 2>/dev/null; then
      echo ""
      echo "[OK] attempt $attempt caught an EnsureFailed; capture saved"
      break
   fi

   echo "[INFO] attempt $attempt finished without throw; retrying..."
   attempt=$((attempt + 1))
done

echo ""
echo "============================================================"
echo "RESULTS"
echo "============================================================"
ls -la "$OUT_DIR"
echo ""
echo "Inspect:"
echo "  $OUT_DIR/gdb_capture.log     # 'N4-walker' lines have the key + dt_id"
echo "  $OUT_DIR/stdout_attempt*.log # raw stdout including 'Throwing exception' line"
