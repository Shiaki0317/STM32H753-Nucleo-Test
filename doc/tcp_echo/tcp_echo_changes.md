# TCP Echo変更内容・実装解説書

## 1. 全体像

今回の変更では、既存のEthernet DMA、D-Cache、lwIP初期化を維持したまま、lwIP Raw APIを使うTCP Echoサーバーを追加した。

```text
MX_LWIP_Init()
  ├─ lwip_init()
  ├─ netif_add()
  ├─ netif_set_up()
  └─ TCP_Echo_Init()
       ├─ tcp_new_ip_type(IPv4)
       ├─ tcp_bind(port 7)
       ├─ tcp_listen()
       └─ tcp_accept(callback登録)

TCP connection
  ├─ accept  → 接続単位の状態を確保
  ├─ receive → 受信pbufを送信queueへcopy
  ├─ sent    → 送信領域が空いたら残りを再送
  ├─ poll    → 一時的な送信待ちとcloseを再試行
  └─ error   → pbufと接続状態を解放
```

## 2. 変更ファイル

### 2.1 `LWIP/App/tcp_echo.h`

追加した公開interfaceである。

```c
#define TCP_ECHO_PORT 7U
err_t TCP_Echo_Init(void);
```

port番号を1か所へ集約し、lwIP初期化側は実装内部を知らずにサーバーを開始できる。

### 2.2 `LWIP/App/tcp_echo.c`

TCP Echoサーバー本体を新規追加した。主な機能は次のとおりである。

- IPv4のTCP port 7でlistenする。
- 接続ごとにpending pbuf、処理offset、close待ち状態を管理する。
- 受信したbyte列を`TCP_WRITE_FLAG_COPY`でTCP送信queueへ渡す。
- 送信領域が不足した場合は、未処理位置を保持する。
- ACK受信後の`sent` callbackで残りを再試行する。
- TCP timerの`poll` callbackでも再試行する。
- peerからFINを受けた場合、受信済みデータをすべてEchoしてからcloseする。
- resetや異常終了時にpbufと接続状態を解放する。

### 2.3 `LWIP/App/lwip.c`

CubeMXのUSER CODE領域へ次を追加した。

```c
#include "tcp_echo.h"
```

`MX_LWIP_Init()`の最後でサーバーを開始する。

```c
if (TCP_Echo_Init() != ERR_OK)
{
  Error_Handler();
}
```

lwIP coreとnetwork interfaceの初期化後に呼ぶことで、TCP PCB用memory poolなどを利用可能な状態にしてからlistenを開始する。

## 3. `.ioc`を変更していない理由

既存設定で次が有効なため、TCP Echo追加に`.ioc`変更は不要だった。

```text
lwIP              enabled
LWIP_TCP          enabled（lwIP default）
NO_SYS            1
LWIP_NETCONN      0
LWIP_SOCKET       0
IPv4              192.168.2.10/24
```

Socket APIではなくRaw APIを使うため、FreeRTOS、Netconn API、Socket APIを追加する必要がない。

## 4. Raw APIを選んだ理由

現在のprojectはRTOSなしの`NO_SYS=1`である。lwIP Raw APIはcallback方式であり、現在のメインループ構成へ自然に組み込める。

| API | 現在の構成との適合性 |
|---|---|
| Raw API | `NO_SYS=1`で利用可能。軽量だがcallback設計が必要 |
| Netconn API | OS mailbox/threadを前提とするため現在は無効 |
| Socket API | Netconn/OS構成を前提とするため現在は無効 |

Raw API callbackは`MX_LWIP_Process()`から呼ばれるlwIP処理文脈で実行される。callback内でdelay、無限待ち、重い処理を行うとEthernet受信とTCP timer全体を止めるため、処理は短く保つ。

## 5. 接続単位の状態管理

```c
typedef struct
{
  struct pbuf *pending;
  u8_t close_pending;
} TCP_EchoConnection_t;
```

### `pending`

TCP送信queueへまだ渡せていない受信データを保持する。受信callbackが受け取ったpbufの所有権をEcho実装へ移す。

### `close_pending`

peerからFINを受けたが、Echoすべきデータが残っている状態を表す。pendingが空になってから`tcp_close()`する。

## 6. callbackの意味

### 6.1 Accept callback

新しいTCP接続ごとに`TCP_EchoConnection_t`をlwIP heapから確保し、receive、sent、error、poll callbackを登録する。

確保できない場合は接続をabortする。状態なしで接続を継続すると、受信pbufの所有権や再送位置を管理できないためである。

### 6.2 Receive callback

`p != NULL`はdata受信、`p == NULL`はpeerが送信側をcloseしたことを表す。

data受信時はpending chainへ追加して`TCP_Echo_Flush()`を呼ぶ。受信窓は、dataをTCP送信queueへ安全にcopyできたbyte数だけ`tcp_recved()`で戻す。

### 6.3 Sent callback

Echo dataがpeerにACKされると、TCP送信bufferに空きができる。その機会にpending dataの送信を再開する。

### 6.4 Poll callback

一時的な`ERR_MEM`で送信やcloseができなかった場合の再試行経路である。ACKが来ない状況でもTCP timerから再度処理できる。

### 6.5 Error callback

このcallbackが呼ばれた時点でTCP PCBはlwIPによって解放済みである。PCBへ再アクセスせず、アプリケーションが所有するpbufと接続状態だけを解放する。

## 7. `TCP_Echo_Flush()`の意味

`TCP_Echo_Flush()`はEcho処理の中心である。

1. `tcp_sndbuf()`で現在の送信可能byte数を取得する。
2. pending先頭pbufの長さと送信可能量の小さい方を求める。
3. その範囲を`tcp_write()`へ渡す。
4. `TCP_WRITE_FLAG_COPY`でlwIP送信queueへcopyする。
5. copy成功分を`tcp_recved()`で受信済みとして通知する。
6. copy済みの先頭dataを`pbuf_free_header()`で逐次解放する。
7. `tcp_output()`で送信を促す。
8. FIN受信済みなら、すべてEchoした後に接続をcloseする。

受信pbufはEthernet DMAのzero-copy RX poolから供給される。chain全体の処理完了まで先頭pbufを保持すると、連続受信時にcopy済みpbufまでpoolへ戻らず、数KiBでRX poolが枯渇する。処理済み部分を逐次解放することで、DMAが次の受信bufferとして再利用できる。

### `TCP_WRITE_FLAG_COPY`を使う理由

copyしない方式では、TCP ACKが返るまで元の受信bufferを変更・解放できない。本実装は送信queueへcopyするため、queue投入完了後に受信pbufを解放でき、bufferの寿命管理が明確になる。

代わりにRAM間copyのCPU負荷が発生する。評価用Echoと現在の通信規模では、安全性と実装の明確さを優先した。

## 8. Ethernet RX pool復旧通知

`LWIP/Target/ethernetif.c`の`pbuf_free_custom()`へ次を追加した。

```c
if (RxAllocStatus == RX_ALLOC_ERROR)
{
  RxAllocStatus = RX_ALLOC_OK;
  RxDataAvailable = 1U;
}
```

RX pool枯渇時はEthernet DMAがRBU（Receive Buffer Unavailable）になる。pbufがpoolへ返された時点でメインループへ再通知し、受信descriptorを再構築する機会を作る。従来はstatusだけを正常へ戻していたため、DMA停止後に新しい受信割り込みが来ず、復旧処理が再実行されない可能性があった。

HALの`heth.ErrorCode`は履歴として`HAL_ETH_ERROR_DMA`を保持する場合があるため、値だけで現在も停止中とは判断しない。`RxAllocStatus`、DMA status、受信再開、ping応答を合わせて確認する。

## 9. 正常closeとabortの違い

| 処理 | 使用場面 | TCP上の意味 |
|---|---|---|
| `tcp_close()` | 正常終了 | 未送信dataを考慮し、FINで終了する |
| `tcp_abort()` | 状態欠損や回復不能error | RSTで即時終了する |

`tcp_close()`が一時的に`ERR_MEM`を返した場合は状態を残し、poll/sent callbackで再試行する。

## 10. 制約と今後の拡張

- 認証、暗号化、接続元制限はない。
- listen対象は全IPv4 addressである。
- active connection数はlwIPのTCP PCB poolに制限される。
- 大量接続や高速連続通信ではheap、pbuf、TCP window、descriptor数の再評価が必要である。
- 大容量Echoのclientは送信と受信を並行させ、TCPの全二重通信とflow controlを妨げないようにする。
- 製品機能にする場合はport変更、接続timeout、最大受信量、統計、ログ、アクセス制御を追加する。
- TLSが必要なら単純なEcho Raw APIとは別に、TLS libraryのmemoryとtimer設計が必要になる。

## 11. ソース対応表

| 処理 | 関数 |
|---|---|
| サーバー開始 | `TCP_Echo_Init()` |
| 接続受付 | `TCP_Echo_Accept()` |
| data/FIN受信 | `TCP_Echo_Receive()` |
| ACK後の再送 | `TCP_Echo_Sent()` |
| timer再試行 | `TCP_Echo_Poll()` |
| 異常終了 | `TCP_Echo_Error()`、`TCP_Echo_Abort()` |
| Echo queue投入 | `TCP_Echo_Flush()` |
| pbuf/state解放 | `TCP_Echo_FreeConnection()` |
