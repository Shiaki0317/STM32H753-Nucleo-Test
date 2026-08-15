# STM32H753 TCP Echo実装資料

## 1. 全体像

このフォルダは、NUCLEO-H753ZI上のlwIPへTCP Echoサーバーを追加した変更内容、実装の意味、導入・確認手順、PC側テストコードをまとめたものである。

```text
PC TCP client
  │ 192.168.2.10:7へ接続
  │ 任意のbyte列を送信
  ▼
STM32 Ethernet DMA
  ▼
lwIP TCP
  ▼
TCP_Echo_Receive()
  │ 受信データをTCP送信queueへcopy
  ▼
同じbyte列をPCへ返送
```

TCP Echoは、クライアントから受信したデータを変更せず、そのまま同じTCP接続へ返す機能である。本実装はEcho Protocolの標準portであるTCP `7`を使用する。

## 2. 資料一覧

| ファイル | 内容 |
|---|---|
| [tcp_echo_changes.md](./tcp_echo_changes.md) | 変更ファイル、処理構造、各callbackを変更した理由 |
| [tcp_echo_procedure.md](./tcp_echo_procedure.md) | ビルド、書き込み、netcat、自動テスト、デバッグ手順 |
| [test_tcp_echo.py](./test_tcp_echo.py) | PCからEcho応答を自動検証するテストコード |

## 3. 実装情報

| 項目 | 内容 |
|---|---|
| Branch | `feature/tcp_echo` |
| Base implementation commit | `39ac905 Add lwIP TCP echo server` |
| Server address | `192.168.2.10` |
| TCP port | `7` |
| lwIP API | Raw API |
| OS構成 | RTOSなし、`NO_SYS=1` |
| IPv4設定 | 固定IP `192.168.2.10/24` |
| 対応データ | textおよびbinary |
| 送信待ち | `sent` callbackと`poll` callbackで再試行 |
| 複数接続 | 接続ごとに状態を確保 |

## 4. 実機確認結果

2026-08-15に次を確認した。

- STM32CubeIDE Debug build：0 errors
- `tcp_echo.c`単体：`-Wall -Wextra -Werror`で警告・エラーなし
- `TCP_Echo_Init()`：`ERR_OK`
- listen PCB：生成成功
- listen port：TCP `7`
- `heth.ErrorCode`：`0`
- ping：4送信、4応答、packet loss 0%
- netcat：文字列のEcho応答を確認
- 自動試験：4,096 byteのbinary payloadを3接続連続で送信
- 自動試験結果：全接続で受信4,096 byte、内容完全一致
- stress試験：16,384 byteを10接続連続で送受信し、全byte一致
- stress後ping：6送信、6応答、packet loss 0%
- stress後RX状態：`RxAllocStatus=0`、active PCBなし、listen PCB継続

stress試験中には負荷によりEthernet DMAのRBUが発生し、`heth.ErrorCode`へ`HAL_ETH_ERROR_DMA`がstickyに残る場合がある。pbuf返却時の再通知によってRX descriptorを再構築し、通信が継続・復旧することを確認している。

## 5. 最短の確認方法

ボードへfirmwareを書き込み、PCを同じ`192.168.2.0/24`へ接続した後、次を実行する。

```bash
ping -c 4 -W 2 192.168.2.10
python3 doc/tcp_echo/test_tcp_echo.py
```

成功時の例：

```text
target=192.168.2.10:7 payload_size=4096 repeat=3
text: sent=... received=... match=yes
binary[1/3]: sent=4096 received=4096 match=yes
binary[2/3]: sent=4096 received=4096 match=yes
binary[3/3]: sent=4096 received=4096 match=yes
PASS: all TCP echo tests succeeded
```

## 6. 注意事項

- Echoサーバーは認証を行わず、全IPv4 interfaceのTCP port 7で待ち受ける。
- 評価用機能であり、そのまま外部公開ネットワークへ接続しない。
- `NO_SYS=1`のため、`MX_LWIP_Process()`をメインループから継続して呼ぶ。
- Echo callback内で長時間待機する処理を追加しない。
- TCP送信queueが一杯のとき、受信pbufを一時保持するためlwIP heapを消費する。
