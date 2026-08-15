# TCP Echo導入・動作確認手順書

## 1. 全体像

この手順書は、`feature/tcp_echo`のfirmwareをビルドしてNUCLEO-H753ZIへ書き込み、PCからTCP port 7へ接続し、Echo応答を確認するまでを説明する。

```text
1. branchとsourceを確認
2. STM32CubeIDEでbuild
3. ST-LINK経由でboardへ書き込み
4. PHY linkとpingを確認
5. netcatで手動Echo確認
6. test_tcp_echo.pyで自動比較
7. debuggerで内部状態を確認
```

## 2. 前提条件

| 項目 | 設定 |
|---|---|
| Board | NUCLEO-H753ZI |
| Board IPv4 | `192.168.2.10/24` |
| PC IPv4例 | `192.168.2.150/24` |
| TCP port | `7` |
| 接続 | 同一LANまたは直接Ethernet接続 |
| Build tool | STM32CubeIDE 1.19.0 |
| Test tool | Python 3、任意でnetcat |

PCとboardのIP addressは重複させない。PC側firewallでTCP port 7のclient通信を妨げていないことも確認する。

## 3. branchと変更内容を確認する

```bash
git branch --show-current
git status -sb
git log -1 --oneline
```

期待値：

```text
feature/tcp_echo
39ac905 Add lwIP TCP echo server
```

主要sourceが存在することを確認する。

```text
LWIP/App/tcp_echo.c
LWIP/App/tcp_echo.h
LWIP/App/lwip.c
```

## 4. STM32CubeIDEでビルドする

### GUIから行う場合

1. STM32CubeIDEでprojectを開く。
2. Project > Cleanを実行する。
3. Project > Build Projectを実行する。
4. Consoleで`Build Finished`と`0 errors`を確認する。
5. `Debug/stm32h753_Nucleo.elf`が更新されたことを確認する。

新規`tcp_echo.c`がbuild対象へ認識されない場合は、projectをRefreshしてからclean buildする。

### 確認するsymbol

```bash
arm-none-eabi-nm -S Debug/stm32h753_Nucleo.elf | grep TCP_Echo_Init
```

`TCP_Echo_Init`のsymbolが表示されれば、Echo実装がELFへlinkされている。

## 5. boardへ書き込む

1. NUCLEOのST-LINK USBをPCへ接続する。
2. LAN cableを接続する。
3. STM32CubeIDEでDebugを開始する。
4. `main()`まで実行できることを確認する。
5. Resumeしてmain loopを継続動作させる。

Debug停止中は`MX_LWIP_Process()`も停止するため、pingやTCP応答は返らない。通信試験中は必ずResume状態にする。

## 6. listener初期化を確認する

`TCP_Echo_Init()`へbreakpointを置き、step overまたはfinish後に次を確認する。

| 確認項目 | 期待値 |
|---|---|
| 戻り値 | `ERR_OK`（0） |
| `TCP_EchoListenPcb` | `NULL`以外 |
| `TCP_EchoListenPcb->local_port` | `7` |
| `heth.ErrorCode` | `0` |

確認後はbreakpointを解除してResumeする。

## 7. Ethernet疎通を確認する

PC側interfaceを確認する。

```bash
ip -brief address
```

boardへpingする。

```bash
ping -c 4 -W 2 192.168.2.10
```

期待値：4 packetすべてへ応答し、packet lossが0%になる。

pingに失敗する場合、TCPより先にPHY link、RMII、DMA、D-Cache、IP addressを確認する。TCP Echoの問題と判断するのはping成功後である。

## 8. netcatで手動確認する

```bash
nc -v 192.168.2.10 7
```

接続後に任意の文字列を入力する。

```text
STM32 TCP Echo Test
```

同じ文字列が返れば成功である。terminalのlocal echo設定によっては、入力表示と返送表示で同じ行が2回見える場合がある。

終了は`Ctrl+C`を使用する。

## 9. 自動テストを実行する

repository rootで次を実行する。

```bash
python3 doc/tcp_echo/test_tcp_echo.py
```

defaultでは次を検証する。

- `192.168.2.10:7`へTCP接続できる。
- UTF-8 textが完全一致で返る。
- 4,096 byteの決定的binary dataが完全一致で返る。
- binary試験を3回、接続し直して成功する。
- clientの送信側close後も全Echo dataが返る。

テストコードはTCPの送信と受信を`select`で並行して進める。全dataの送信完了を待ってから受信を始める方式では、payloadがTCP windowより大きい場合、clientとserverの双方がflow control待ちになる可能性があるためである。

### parameterを変更する

```bash
python3 doc/tcp_echo/test_tcp_echo.py \
  --host 192.168.2.10 \
  --port 7 \
  --size 16384 \
  --repeat 10 \
  --timeout 5
```

大きなsizeやrepeatは、TCP window、lwIP heap、pbuf pool、処理速度の安定性確認に使える。

本実装では`--size 16384 --repeat 10 --timeout 5`が全接続で完全一致し、試験後のping 6回もpacket loss 0%となることを実機確認している。

## 10. 期待するテスト結果

```text
target=192.168.2.10:7 payload_size=4096 repeat=3
text: sent=... received=... match=yes
binary[1/3]: sent=4096 received=4096 match=yes
binary[2/3]: sent=4096 received=4096 match=yes
binary[3/3]: sent=4096 received=4096 match=yes
PASS: all TCP echo tests succeeded
```

scriptが異常を検出した場合は終了code 1となり、接続error、timeout、受信長、最初に異なるbyte位置を表示する。

## 11. callbackをdebuggerで確認する

必要に応じて次へbreakpointを置く。

| 関数 | 確認できること |
|---|---|
| `TCP_Echo_Accept()` | TCP handshake完了と接続受付 |
| `TCP_Echo_Receive()` | dataまたはFINの受信 |
| `TCP_Echo_Flush()` | pbufからTCP送信queueへのcopy |
| `TCP_Echo_Sent()` | peer ACKと送信領域の解放 |
| `TCP_Echo_Poll()` | timerによる再試行 |
| `TCP_Echo_Error()` | resetなどの異常終了 |

breakpointで長時間停止するとTCP client側がtimeoutする。状態を確認したら速やかにResumeする。

## 12. 症状別トラブルシューティング

| 症状 | 確認項目 |
|---|---|
| pingできない | PHY link、RMII、DMA、cache、PC/board IP |
| ping成功、port 7へ接続できない | `TCP_Echo_Init()`戻り値、listen PCB、firmware書き込み |
| 接続直後にresetされる | connection state用lwIP heap、TCP PCB pool |
| 短文だけ成功する | `tcp_sndbuf()`、sent/poll callback、copy済みpbufの逐次解放 |
| 一部dataが欠ける | pbuf chain、`tcp_write()`戻り値、受信長比較 |
| close時に末尾が欠ける | `close_pending`、pending処理完了前のclose |
| 数回後に接続不能 | pbuf/state/PCB解放、error callback、heap枯渇 |
| pingを含め全受信が停止 | RX pool、RBU、`pbuf_free_custom()`からの再通知 |
| debugger中だけtimeout | breakpoint停止時間、main loopの停止 |

## 13. CubeMX再生成時の確認

`tcp_echo.c`と`tcp_echo.h`はCubeMX生成対象外の独立ファイルである。`lwip.c`へのincludeと`TCP_Echo_Init()`呼び出しはUSER CODE領域にあるため、通常は再生成後も保持される。

再生成後は必ず差分を確認し、clean buildと実機自動テストを再実行する。

## 14. 完了条件

- buildが0 errorsで完了する。
- `TCP_Echo_Init()`が`ERR_OK`を返す。
- TCP port 7でlistenする。
- ping packet lossが0%である。
- text Echoが完全一致する。
- 4,096 byte以上のbinary Echoが完全一致する。
- 複数回の再接続で失敗しない。
- 接続終了後も次の接続を受け付ける。
