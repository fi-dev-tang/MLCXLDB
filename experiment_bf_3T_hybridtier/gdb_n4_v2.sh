#!/usr/bin/env bash
# =============================================================================
# N4 round 2 GDB capture: after the delivery c_id init fix, the crash STILL
# fires WITHOUT "delivery skipped" warning. So either:
#   (a) order.lookup1 returns OK with a corrupted rec.o_c_id (not NOT_FOUND),
#   (b) update1 NOT_FOUND fires from a totally different call site.
#
# This script catches the throw and dumps:
#   - which adapter type (this->btree → table identity via dt_id)
#   - the full folded_key
#   - the caller frame (which tpcc tx + key params)
#   - extra: walk up to 50 frames to find any TPCCWorkload:: frame
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"
BINARY="$BUILD_DIR/tpcc_compare_test"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_n4_gdb_v2"
CXL_DAX_DEVICE="/dev/dax0.1"

MAX_RETRY="${MAX_RETRY:-3}"

TS=$(date +%Y%m%d_%H%M%S)
OUT_DIR="$SCRIPT_DIR/gdb_n4_v2_${TS}"
mkdir -p "$OUT_DIR"

if [[ ! -x "$BINARY" ]]; then
   echo "[ERR] binary not found: $BINARY" >&2
   exit 1
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

catch throw
commands
  silent
  printf "\n================ throw caught ================\n"
  shell date '+wall %Y-%m-%d %H:%M:%S.%N'
  printf "\n--- thread ---\n"
  thread
  printf "\n--- backtrace top 50 ---\n"
  bt 50
  printf "\n--- python walker: find update1 frame + caller ---\n"
python
import gdb
try:
    f = gdb.newest_frame()
    depth = 0
    update1_frame = None
    update1_depth = -1
    caller_frame = None
    caller_depth = -1
    while f is not None and depth < 50:
        sym = f.function()
        name = sym.print_name if sym else '<no-sym>'
        if name and 'update1' in name and 'Adapter' in name and update1_frame is None:
            update1_frame = f
            update1_depth = depth
            print('[walker] update1 frame at depth', depth, ':', name)
        elif update1_frame is not None and caller_frame is None and name and 'TPCCWorkload' in name:
            caller_frame = f
            caller_depth = depth
            print('[walker] caller frame at depth', depth, ':', name)
            break
        f = f.older()
        depth += 1
    if update1_frame is not None:
        update1_frame.select()
        try:
            k_len = int(gdb.parse_and_eval('folded_key_len'))
            print('[walker] folded_key_len =', k_len)
            bytes_hex = ''
            for i in range(min(k_len, 64)):
                bytes_hex += '%02x ' % (int(gdb.parse_and_eval('folded_key[%d]' % i)) & 0xff)
            print('[walker] folded_key =', bytes_hex)
        except Exception as e:
            print('[walker] err dumping key:', repr(e))
        try:
            t = gdb.parse_and_eval('*this').type
            print('[walker] *this type =', str(t))
        except Exception as e:
            print('[walker] err dumping this type:', repr(e))
        try:
            key_val = gdb.parse_and_eval('key')
            print('[walker] key value:', str(key_val))
        except Exception as e:
            print('[walker] err dumping key value:', repr(e))
    if caller_frame is not None:
        caller_frame.select()
        try:
            gdb.execute('info args', to_string=False)
        except Exception as e:
            print('[walker] err info args:', repr(e))
        try:
            gdb.execute('info locals', to_string=False)
        except Exception as e:
            print('[walker] err info locals:', repr(e))
    if update1_frame is None:
        print('[walker] no update1 frame found')
except Exception as e:
    print('[walker] outer exception:', repr(e))
end
  printf "\n================ end capture ================\n"
  continue
end

catch signal SIGABRT
commands
  silent
  printf "\n================ SIGABRT (terminate) ================\n"
  bt 30
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

sed -i "s|__OUT_DIR__|$OUT_DIR|g; s|__SSD_PATH__|$SSD_PATH|g; s|__CXL_DAX_DEVICE__|$CXL_DAX_DEVICE|g" "$GDB_SCRIPT"

attempt=1
while (( attempt <= MAX_RETRY )); do
   echo ""
   echo "============================================================"
   echo "[INFO] attempt $attempt / $MAX_RETRY  ($(date '+%H:%M:%S'))"
   echo "[INFO] out dir   : $OUT_DIR"
   echo "============================================================"

   rm -f "$SSD_PATH"

   gdb -batch -x "$GDB_SCRIPT" "$BINARY" 2>&1 | tee "$OUT_DIR/stdout_attempt${attempt}.log"

   if grep -q "\[walker\]" "$OUT_DIR/gdb_capture.log" 2>/dev/null; then
      echo "[OK] attempt $attempt caught; capture saved"
      break
   fi

   echo "[INFO] attempt $attempt finished without catching; retrying..."
   attempt=$((attempt + 1))
done

echo ""
echo "============================================================"
echo "RESULTS"
echo "============================================================"
ls -la "$OUT_DIR"
echo ""
echo "Key lines:"
grep -E "\[walker\]|throw caught|update1|TPCCWorkload" "$OUT_DIR/gdb_capture.log" 2>/dev/null | head -40
