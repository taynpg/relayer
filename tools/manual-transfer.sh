#!/usr/bin/env bash
# 一次性跑通三进程传输的手工验证脚本。
#
# 它不是 CTest 的一部分——tests/e2e 才是。这里存在的理由是"手动复现一个
# 具体场景时，不想每次重新敲五条命令"，而脚本化之后我至少能确定复现步骤
# 本身没写错。
#
# 注意：relayerd / relayer-cli 是**原生 Windows 程序**，不认识 MSYS 的
# /tmp。所以所有传给它们的路径都要先过 cygpath -w，否则会得到
# "source file does not exist"——而那个报错看上去像文件真的不存在。
#
# --- 两个踩过两次的坑，写在这里免得再踩 ---
#
# 1. **接收端的 id 必须从 ASSIGNED_ID= 契约行取，不能用 list 去猜。**
#    id 由服务端递增分配，而每条新连接都消耗一个号——包括 list 自己那条。
#    所以 `list | grep client- | head -1` 拿到的几乎总是 list 自己，然后
#    发送端把文件投给一个刚断开的连接，中继计 unroutable 静默丢弃，症状
#    是"发送端等应答超时"，与网络问题完全一样。
#
# 2. **契约行只取第一条非空且完整的。** relayer-cli 会在"认证成功但还
#    不知道自己叫什么"时先回调一次，那一行是 `ASSIGNED_ID=`（空值）。
#    用 head -1 会拿到空串。现在 cli 侧已经不再打印空值了，但这里仍然
#    做一次防御性过滤，免得将来又退回老行为。
set -u

BIN=${BIN:-/d/Code/relayer/build/bin}
PORT=${PORT:-9560}
WORK_MSYS=${WORK:-/tmp/rt3}
PW=testpw
SIZE=${SIZE:-2000000}

rm -rf "$WORK_MSYS"
mkdir -p "$WORK_MSYS/dst"

# 造测试文件。每次重建工作目录之后都要重新造——早先的版本让调用者预先
# 放一个 src.bin 进去，而脚本第一行就把它删了，于是 send 报
# "source file does not exist"。
head -c "$SIZE" /dev/urandom > "$WORK_MSYS/src.bin"

# 之后凡是交给子进程的路径，一律用这个。
WORK=$(cygpath -w "$WORK_MSYS")

echo "== 启动服务端 (:$PORT) =="
"$BIN/relayerd.exe" --port "$PORT" --password "$PW" > "$WORK_MSYS/server.log" 2>&1 &
SRV=$!

# 等服务端真的开始 listen，而不是盲等固定秒数。盲等有两种失败方式：
# 等太短会连不上（"目标机器积极拒绝"），等太长白耗时间；而两者都只在
# 慢机器上偶发，最难查。
for _ in $(seq 1 50); do
    grep -q "已启动" "$WORK_MSYS/server.log" 2>/dev/null && break
    sleep 0.1
done
if ! grep -q "已启动" "$WORK_MSYS/server.log" 2>/dev/null; then
    echo "失败：服务端没起来"
    cat "$WORK_MSYS/server.log"
    exit 1
fi

echo "== 启动接收端 =="
"$BIN/relayer-cli.exe" recv --to-dir "$WORK\\dst" -p "$PORT" --password "$PW" \
    --idle-timeout 30 > "$WORK_MSYS/recv.log" 2>&1 &
RCV=$!

# 等契约行出现。用"非空且完整"的模式匹配，空值那行天然不匹配。
RECV_ID=""
for _ in $(seq 1 100); do
    RECV_ID=$(tr '\r' '\n' < "$WORK_MSYS/recv.log" | grep -oE 'ASSIGNED_ID=client-[0-9]+' | head -1 | cut -d= -f2)
    [ -n "$RECV_ID" ] && break
    sleep 0.1
done
echo "接收端 id = ${RECV_ID:-<未拿到>}"

if [ -z "$RECV_ID" ]; then
    echo "失败：接收端没拿到 id"
    tr '\r' '\n' < "$WORK_MSYS/recv.log" | head -5
    kill $RCV $SRV 2>/dev/null
    exit 1
fi

echo "== 发送 =="
"$BIN/relayer-cli.exe" send "$WORK\\src.bin" --to "$RECV_ID" -p "$PORT" --password "$PW" \
    --chunk-size 262144 --idle-timeout 30 --hard-timeout 120 2>&1 | tr '\r' '\n' | tail -3
SEND_RC=${PIPESTATUS[0]}

echo "== 等接收端结束 =="
for _ in $(seq 1 60); do
    if ! kill -0 $RCV 2>/dev/null; then break; fi
    sleep 0.5
done
wait $RCV 2>/dev/null
RCV_RC=$?

echo
echo "发送退出码=$SEND_RC  接收退出码=$RCV_RC"
echo "--- 接收端结论 ---"
tr '\r' '\n' < "$WORK_MSYS/recv.log" | grep -v "^$" | grep -v "%" | tail -3

echo "--- 内容比对 ---"
if [ -f "$WORK_MSYS/dst/src.bin" ]; then
    if cmp -s "$WORK_MSYS/src.bin" "$WORK_MSYS/dst/src.bin"; then
        echo "OK: 内容一致"
    else
        echo "FAIL: 内容不一致"
    fi
    # 残片不该留下：它留在盘上说明收尾没走完。
    if [ -f "$WORK_MSYS/dst/src.bin.relaypart" ]; then
        echo "FAIL: 留下了 .relaypart 残片"
    fi
else
    echo "FAIL: 目标文件不存在"; ls -la "$WORK_MSYS/dst"
fi

kill $SRV 2>/dev/null
