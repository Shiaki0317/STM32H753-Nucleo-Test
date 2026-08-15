# STM32H753のlwIP設定 解説書

## 1. 全体像

### 1.1 lwIPとは何か

lwIP（lightweight IP）は、メモリの限られた組み込み機器向けのTCP/IPプロトコルスタックである。LANケーブルの電気信号を直接扱うものではなく、Ethernetフレーム内のARP、IPv4、ICMP、UDP、TCPなどを解釈・生成する。

```text
アプリケーション        ping応答、UDP/TCPアプリなど
────────────────────────────────────────
lwIP                    ICMP / UDP / TCP
                        IPv4 / ARP
────────────────────────────────────────
ethernetif.c            lwIPとSTM32 HALの変換層
────────────────────────────────────────
STM32 ETH MAC + DMA     EthernetフレームとRAM転送
────────────────────────────────────────
RMII + LAN8742A         デジタル信号とLANケーブル信号の変換
```

設定を理解するときは、「IPの設定」「lwIPの実行方式」「メモリ」「MAC/DMA」「PHY」を別々に考え、最後につなげると分かりやすい。

### 1.2 ping応答までに起こること

1. PCは`192.168.2.10`のMACアドレスを知らないためARP要求を送る。
2. LAN8742Aが電気信号をRMII信号へ変換する。
3. STM32のMACがフレームを認識し、DMAが受信バッファへ置く。
4. DMA完了割り込みが受信フラグを立てる。
5. メインループが`ethernetif_input()`を呼ぶ。
6. lwIPがARP応答を作り、PCがボードのMACアドレスを記憶する。
7. PCがICMP Echo Requestを送る。
8. lwIPがICMP Echo Replyを作る。
9. MAC/DMA、RMII、PHYを逆向きに通ってPCへ戻る。

pingはアプリケーションコードを追加しなくても、lwIPのIPv4/ICMP機能が正しく動けば応答できる。

## 2. IP設定の意味

### 2.1 IPアドレス

`192.168.2.10`はネットワーク上でボードを識別するIPv4アドレスである。同じLAN内で重複すると、どちらへ送るべきか決められず通信が不安定になる。

### 2.2 ネットマスク

`255.255.255.0`（`/24`）は、先頭24ビットの`192.168.2`がネットワーク部、最後の8ビットが機器部であることを表す。

```text
ボード  192.168.2.10/24 ┐
PC      192.168.2.20/24 ├ 同一ネットワーク → 直接ARPして通信
GW      192.168.2.1/24  ┘
```

PCが`192.168.1.20/24`なら別ネットワークである。ルーター設定がなければ直接pingできない。

### 2.3 ゲートウェイ

ゲートウェイは、ボードと異なるネットワークへ送るときの出口である。同一セグメントでの直結pingには使われない。将来インターネットや別サブネットへ接続するなら、実在するルーターのアドレスにする。

### 2.4 固定IPとDHCP

| 方式 | 長所 | 注意点 |
|---|---|---|
| 固定IP | 接続先が常に同じで、評価しやすい | 重複防止とネットワーク管理が必要 |
| DHCP | ネットワークから自動取得できる | DHCPサーバーが必要、起動ごとに変わる場合がある |

本プロジェクトは再現性を優先して固定IPにしている。

## 3. lwIPの初期化設定の意味

### 3.1 `lwip_init()`

lwIP内部のメモリ、プロトコル、タイマーなどを初期化する。ネットワークインターフェースを登録する前に一度だけ呼ぶ。

### 3.2 `netif_add()`

`netif`はlwIPから見た1個のネットワーク装置である。次の呼び出しは、IP設定とSTM32固有の初期化関数を`gnetif`へ結び付ける。

```c
netif_add(&gnetif, &ipaddr, &netmask, &gw,
          NULL, &ethernetif_init, &ethernet_input);
```

- `ethernetif_init`：MAC、DMA、PHYなど下位層を初期化する。
- `ethernet_input`：受信EthernetフレームをARPやIPへ振り分ける。

### 3.3 `netif_set_default()`

送信先に明示的なインターフェース指定がないとき、このインターフェースを使うようにする。Ethernetが1個だけでも設定しておくのが基本である。

### 3.4 interface upとlink up

この2つは別の状態である。

| 状態 | 意味 |
|---|---|
| interface up | ソフトウェア上、インターフェースを使用可能にした |
| link up | PHYがケーブル接続と通信条件の確立を確認した |

IP設定が正しくてもlink downなら送受信できない。逆にリンクLEDが点灯しても、lwIPやIP設定が正しいとは限らない。

## 4. 実行方式の意味

### 4.1 `NO_SYS=1`

```c
#define WITH_RTOS 0
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
```

これはOSなしのメインループ方式を表す。lwIPのraw APIとタイマー処理は、原則として同じ実行コンテキストから呼ぶ。

公式のNO_SYSの説明でも、受信パケットはメインループからスタックへ渡し、ISRから直接呼ばないこと、`sys_check_timeouts()`を周期的に呼ぶことが示されている。

### 4.2 なぜ割り込み内でlwIP処理をしないのか

割り込み内でプロトコル処理を行うと、次の問題が起こりやすい。

- ISRが長くなり、他の割り込みを遅らせる。
- メイン処理と同時にlwIP内部状態へ触れる。
- 排他や再入可能性が複雑になる。
- アプリのコールバックが割り込み文脈で実行される危険がある。

そのため、本プロジェクトのISRは`RxDataAvailable=1`だけを行い、実処理をメインループへ移す。

### 4.3 「受信処理は定期処理か」への正確な答え

完全な定期ポーリングではない。役割ごとに次の方式を使う。

| 処理 | 起点 | 実行場所 |
|---|---|---|
| フレーム到着通知 | ETH DMA割り込み | ISRでフラグ設定 |
| フレームのlwIP入力 | 受信フラグ | メインループ |
| lwIPタイムアウト | メインループごと | `sys_check_timeouts()` |
| PHYリンク確認 | 100 ms経過 | `ethernet_link_check_state()` |

つまり受信は「割り込み通知＋メインループ処理」、リンク監視だけが明示的な100 ms定期処理である。

### 4.4 `sys_check_timeouts()`

ARPの期限、TCP再送、DHCP、各種プロトコルのタイマーを進める。呼び出しが止まると、単純なpingが一時的に動いても、TCPなどが時間経過とともに壊れる可能性がある。

## 5. API設定の意味

```c
#define LWIP_NETCONN 0
#define LWIP_SOCKET  0
```

RTOSなしのため、スレッドやメールボックスを前提とするNetconn APIとSocket APIを無効にしている。この構成ではraw APIを利用する。

raw APIのコールバックはlwIPの処理文脈で呼ばれる。長時間ブロックする処理をコールバック内に置いてはいけない。

将来FreeRTOSとSocket APIを使う場合は、単に2個のマクロを1へ変えるだけでは不十分である。`NO_SYS=0`、tcpip thread、OS抽象化、スレッド間の呼び出し規則を含めて設計し直す。

## 6. メモリ設定の意味

### 6.1 `MEM_ALIGNMENT=4`

lwIPが確保するメモリの基本整列単位である。ただしEthernet受信バッファは、Cortex-M7のD-Cacheラインに合わせて別途32バイト整列している。

### 6.2 lwIP heap

```c
#define LWIP_RAM_HEAP_POINTER 0x30004000
```

lwIPのheapをDMAから利用可能なRAMへ置く。アドレスだけでなく、リンカ設定、他領域との重複、必要容量も合わせて確認する。

### 6.3 pbuf

`pbuf`はlwIPのパケットバッファである。1パケットが複数のpbufにつながる場合もある。

```c
#define LWIP_SUPPORT_CUSTOM_PBUF 1
```

本プロジェクトはcustom pbufを使ったzero-copy受信である。DMA受信バッファを別のlwIPバッファへコピーせず、そのままpbufとして渡すため、CPU負荷とコピー回数を減らせる。一方、バッファの寿命、整列、キャッシュ整合性を正しく管理する必要がある。

### 6.4 RX poolと記述子

```text
RX DMA descriptors: 4
TX DMA descriptors: 4
RX custom buffers:  12
RX buffer size:      1536 bytes
```

DMA記述子は「次にどのバッファを送受信するか」をDMAへ示す管理表である。RX poolは実際のフレーム内容を入れる容器である。poolが枯渇するとDMAが受信バッファを得られず、RBU（Receive Buffer Unavailable）が発生する。

## 7. チェックサム設定の意味

IPv4、UDP、TCPにはデータ破損を検出するチェックサムがある。CPUで計算すればソフトウェアチェックサム、MACで計算すればハードウェアオフロードである。

本プロジェクトは次の組み合わせである。

```text
lwIP側の生成・検査     無効
STM32 ETH MAC側         有効
```

送信時の`TxConfig.ChecksumCtrl`がMACへ計算を指示する。片側だけを無効にすると、不正なパケットを送ったり、正常な受信データを確認しなくなったりするため、常に組で確認する。

WiresharkはPCのNICオフロードの影響で、PC送信パケットのチェックサムを一見不正と表示する場合がある。ボード側だけでなくキャプチャ位置も考慮する。

## 8. Ethernet DMAとD-Cacheの意味

CPUとDMAは同じRAMを見るが、D-Cacheを有効にするとCPUが最新値をキャッシュ内だけに持つ場合がある。

```text
送信: CPUがデータ作成 → D-Cache Clean → RAM → DMA送信
受信: DMAがRAMへ格納 → D-Cache Invalidate → CPU/lwIPが読む
```

- Clean：CPUが変更したキャッシュ内容をRAMへ書き戻す。
- Invalidate：古いキャッシュ内容を捨て、RAMの最新値を次回読み込ませる。
- MPU非キャッシュ領域：DMA記述子のような共有管理情報を常にRAMで一致させる。

これが必要な理由はlwIP固有ではなく、Cortex-M7のCPUキャッシュとEthernet DMAが同じメモリを共有するためである。

## 9. ソースコードを変更した理由

### 9.1 `HAL_ETH_Start_IT()`を使う理由

ETHを割り込みモードで開始し、受信完了をCPUへ通知できるようにするためである。単なる`HAL_ETH_Start()`では、本プロジェクトの受信フラグ方式と整合しない。

### 9.2 `HAL_ETH_RxCpltCallback()`を追加した理由

DMA受信を即座に検知しつつ、ISR内でlwIP処理をしないためである。割り込みとメインループの橋渡しとして`volatile`フラグを使う。

### 9.3 `ethernetif_rx_pending()`を追加した理由

受信フラグを安全に取得してクリアし、受信データがあるときだけ`ethernetif_input()`を呼ぶためである。短時間割り込みを禁止して、読み取りとクリアを一体の操作にしている。

### 9.4 RBU時にも受信処理を促す理由

RX poolが一時的に枯渇した場合、解放されたバッファを使って受信記述子を再構築する機会が必要になる。そのためDMAのRBUエラーでもメインループを受信処理へ進ませる。

### 9.5 キャッシュ操作を追加した理由

D-Cache有効時にCPUとDMAが異なる内容を見ることを防ぐためである。D-Cache無効時は偶然動くが有効時にpingが失敗する、という典型的な不具合を避ける。

## 10. 設定変更時の判断表

| 変更したいこと | 主に変更する場所 | 合わせて確認すること |
|---|---|---|
| IPを変える | `.ioc`のLwIP IPv4 | PC側IP、ネットマスク、重複 |
| DHCPを使う | `.ioc`のDHCP | DHCPサーバー、タイマー、取得完了待ち |
| TCPサーバーを作る | lwIP raw API | callbackでブロックしない、メモリ容量 |
| Socketを使う | RTOS + lwIP設定 | `NO_SYS=0`、tcpip thread、排他 |
| 通信量を増やす | pool、heap、descriptor数 | RAM容量、配置、キャッシュ |
| D-Cacheを有効化 | `.ioc`、MPU、ethernetif | Clean/Invalidate、32 byte整列 |
| PHYを変更 | BSP PHY driver、RMII/MDIO | 回路、PHYアドレス、レジスタ仕様 |

## 11. 関連資料

- [lwIP公式：NO_SYSモード](https://www.nongnu.org/lwip/2_1_x/group__lwip__nosys.html)
- [lwIP公式：NO_SYSオプション](https://lwip.nongnu.org/2_1_x/group__lwip__opts__nosys.html)
- [lwIP公式：netif](https://www.nongnu.org/lwip/2_1_x/group__netif.html)
- [lwIP公式：タイマー設定](https://www.nongnu.org/lwip/2_1_x/group__lwip__opts__timers.html)
- [lwIP公式：マルチスレッド](https://www.nongnu.org/lwip/2_1_x/multithreading.html)
- [ST UM1713：STM32CubeでのlwIPアプリケーション開発](https://www.st.com/resource/en/user_manual/um1713-developing-applications-on-stm32cube-with-lwip-tcpip-stack-stmicroelectronics.pdf)
- `lwip_configuration_procedure.md`
- `stm32h753_ethernet_dma_dcache_explanation.md`
