#!/bin/bash
# =============================================================================
# flamegraph_sample.sh
#
# 用法：
#   1. 先在另一个终端启动 ycsb_c_test，等它打印：
#      [INFO] Enabling deferred background threads (admission + RecordCache)...
#      此时程序卡住，不再有新日志输出。
#
#   2. 在本终端运行本脚本（需要 root 或 sudo 权限）：
#      sudo bash flamegraph_sample.sh <PID>
#
#   3. 脚本结束后会生成 flamegraph_<PID>.svg，用浏览器打开即可。
#
# 原理：
#   perf record -g 以 99 Hz 频率对目标进程采样 30 秒，
#   记录每个采样点的完整调用栈（-g = --call-graph fp，基于帧指针展开）。
#   采样结束后用 FlameGraph 工具链将原始数据渲染成交互式 SVG。
# =============================================================================

set -euo pipefail

FLAMEGRAPH_DIR="/home/zhizhi.tyf/FlameGraph"
OUTPUT_DIR="/home/zhizhi.tyf/cxl-recordcache-dev"
SAMPLE_DURATION=60   # 采样秒数，卡住时 60s 足够捕获热点
SAMPLE_FREQ=99       # 采样频率 Hz（99 避免与定时器中断对齐）

# --------------------------------------------------------------------------
# 参数检查
# --------------------------------------------------------------------------
if [[ $# -lt 1 ]]; then
    echo "用法: sudo bash $0 <PID>"
    echo ""
    echo "示例: sudo bash $0 \$(pgrep ycsb_c_test)"
    exit 1
fi

PID="$1"

if ! kill -0 "$PID" 2>/dev/null; then
    echo "[ERROR] PID $PID 不存在或无权访问"
    exit 1
fi

BINARY=$(readlink -f /proc/"$PID"/exe 2>/dev/null || echo "unknown")
echo "[INFO] 目标进程: PID=$PID, binary=$BINARY"
echo "[INFO] 开始采样 ${SAMPLE_DURATION}s，频率 ${SAMPLE_FREQ} Hz ..."
echo "[INFO] 采样期间请保持目标程序运行（不要 Ctrl+C 目标程序）"

PERF_DATA="${OUTPUT_DIR}/perf_${PID}.data"
FOLDED_FILE="${OUTPUT_DIR}/perf_${PID}.folded"
SVG_FILE="${OUTPUT_DIR}/flamegraph_${PID}.svg"

# --------------------------------------------------------------------------
# Step A: perf record
#   -g              : 记录调用栈（基于帧指针，与二进制编译方式匹配）
#   -F ${FREQ}      : 采样频率
#   -p ${PID}       : 只采样目标进程（所有线程）
#   --sleep         : 采样 N 秒后自动停止
# --------------------------------------------------------------------------
perf record \
    -g \
    -F "${SAMPLE_FREQ}" \
    -p "${PID}" \
    -o "${PERF_DATA}" \
    -- sleep "${SAMPLE_DURATION}"

echo "[INFO] 采样完成，数据保存在: ${PERF_DATA}"

# --------------------------------------------------------------------------
# Step B: perf script — 将二进制采样数据展开为文本调用栈
# --------------------------------------------------------------------------
echo "[INFO] 展开调用栈..."
perf script -i "${PERF_DATA}" > "${OUTPUT_DIR}/perf_${PID}.script"

# --------------------------------------------------------------------------
# Step C: stackcollapse-perf.pl — 折叠调用栈为 FlameGraph 输入格式
# --------------------------------------------------------------------------
echo "[INFO] 折叠调用栈..."
"${FLAMEGRAPH_DIR}/stackcollapse-perf.pl" \
    "${OUTPUT_DIR}/perf_${PID}.script" \
    > "${FOLDED_FILE}"

# --------------------------------------------------------------------------
# Step D: flamegraph.pl — 渲染 SVG
# --------------------------------------------------------------------------
echo "[INFO] 渲染火焰图..."
"${FLAMEGRAPH_DIR}/flamegraph.pl" \
    --title "ycsb_c_test PID=${PID} — enableAdmissionAndRecordCacheThreads 卡顿分析" \
    --subtitle "采样 ${SAMPLE_DURATION}s @ ${SAMPLE_FREQ}Hz" \
    --width 1800 \
    --colors hot \
    "${FOLDED_FILE}" \
    > "${SVG_FILE}"

echo ""
echo "============================================================"
echo "  火焰图生成完毕！"
echo "  SVG 文件: ${SVG_FILE}"
echo "============================================================"
echo ""
echo "查看方式（任选其一）："
echo "  1. 把 SVG 文件下载到本地，用 Chrome/Firefox 打开（支持交互）"
echo "  2. 如果有 X11 转发: firefox ${SVG_FILE}"
echo ""
echo "如何看火焰图："
echo "  - X 轴宽度 = 该函数在所有采样中出现的比例（越宽 = 越耗时）"
echo "  - Y 轴 = 调用栈深度（底部是 main，顶部是叶子函数）"
echo "  - 点击某个函数块可以放大查看其子调用"
echo "  - 重点关注：最宽的顶层函数块，那就是卡顿的根因"
