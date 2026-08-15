# STM32 Ethernet MAC・RMII 初心者〜中級者向け解説書

## 1. 全体像

### 1.1 この資料で理解すること

STM32で有線LAN通信を行うとき、特に混同しやすいのがMAC、PHY、RMII、DMA、lwIPである。本書では、それぞれを次のように分けて説明する。

```text
アプリケーション
  ↓ ping、UDP、TCPなど
lwIP
  ↓ IPパケットをEthernetフレームへ入れる
STM32 Ethernet MAC
  ↓ フレームの送受信、MACアドレス、CRC、速度・duplex
STM32 Ethernet DMA
  ↓ descriptorを使ってRAMとMACの間を転送
RMII
  ↓ STM32 MACと外付けPHYを結ぶデジタル信号
LAN8742A PHY
  ↓ デジタル信号とLANケーブル上の電気信号を変換
LANケーブル／PC
```

最初に覚えるべき点は次の3つである。

1. STM32のMACだけではLANケーブルへ直接接続できない。外付けPHYが必要である。
2. RMIIはMACとPHYの間の接続であり、IP通信プロトコルではない。
3. DMAはフレームをRAMへ運ぶが、その内容をARPやIPとして解釈するのはlwIPである。

### 1.2 現在のプロジェクト構成

| 項目 | 現在の設定 |
|---|---|
| MCU | STM32H753ZITx |
| PHY | LAN8742A（ソフトウェア名は`LAN8742`） |
| MAC-PHY接続 | RMII |
| 通信速度 | Auto-negotiation結果により10/100 Mbps |
| MACアドレス | `02:00:00:00:00:01` |
| IPv4 | `192.168.2.10/24` |
| RX/TX descriptor | 各4個 |
| 受信通知 | ETH DMA割り込み |
| lwIP処理 | RTOSなしのメインループ |
| PHYリンク監視 | 100 ms周期のMDC/MDIO読み出し |

## 2. STM32 Ethernet MACとは

### 2.1 MACの役割

MACはMedia Access Controlの略で、Ethernetのデータリンク層を担当する。STM32H753にはEthernet MACとDMA controllerが内蔵されているが、PHYは内蔵されていない。

MACの主な仕事は次のとおりである。

- 送信するEthernetフレームを組み立てる。
- 宛先MACアドレスを見て受信するフレームを選別する。
- フレームの長さや形式を確認する。
- CRC/FCSを生成または検査する。
- 送信時のpaddingを処理する。
- PHYのリンク条件に合わせて10/100 Mbpsとduplexを設定する。
- checksum offloadなど、CPU負荷を減らす機能を提供する。
- DMA、送受信FIFO、割り込みを介してCPUと連携する。

MACはIPアドレスを判断する中心ではない。IPアドレス、ICMP、UDP、TCPを扱うのはlwIPである。

### 2.2 MACアドレスとIPアドレスの違い

| 項目 | MACアドレス | IPアドレス |
|---|---|---|
| 例 | `02:00:00:00:00:01` | `192.168.2.10` |
| 主な層 | Ethernet | IPv4 |
| 用途 | 同一LAN上でフレームの相手を識別 | ネットワークを越えて通信相手を識別 |
| 現在の担当 | STM32 MAC/lwIP netif | lwIP |

PCが`192.168.2.10`へ初めて送信するとき、ARPを使って「そのIPアドレスを持つ機器のMACアドレス」を問い合わせる。ボードは`02:00:00:00:00:01`を返し、以後PCはそのMACアドレス宛てにEthernetフレームを送る。

現在のMACアドレス先頭の`02`はローカル管理アドレスを表せる値である。同一LAN内で重複しないよう管理する必要がある。

## 3. Ethernetフレームの見方

一般的なEthernetフレームは概念的に次の構造である。

```text
Preamble | SFD | Destination MAC | Source MAC | Type/Length | Payload | FCS
```

| フィールド | 意味 |
|---|---|
| Preamble/SFD | 受信側が同期し、フレーム開始を認識する |
| Destination MAC | フレームの宛先 |
| Source MAC | フレームの送信元 |
| Type | PayloadがIPv4、ARP、IPv6などのどれかを示す |
| Payload | ARPパケットやIPパケットなど |
| FCS | CRCによるフレーム破損検出 |

通常のEthernet MTUは1500バイトである。現在の`ETH_RX_BUFFER_SIZE=1536`は、Ethernetヘッダーなどを含む受信データを格納でき、さらに32バイト単位を扱いやすい大きさになっている。

### 3.1 MACフィルター

MACはすべてのフレームをCPUへ渡す必要はない。一般に次の条件で受信を選別できる。

- 自分のMACアドレス宛て
- Broadcast宛て
- 登録されたMulticast宛て
- Promiscuous modeですべて受信

ARP要求は通常Broadcastで届く。そのため、Broadcastを誤って拒否すると、IP設定が正しくても最初のARP解決が成立しない。

### 3.2 CRCとpadding

Ethernetには最小フレーム長がある。Payloadが短い場合、MACはpaddingを追加できる。また送信末尾のFCSを生成し、受信時にはFCSを検査できる。

現在の送信設定では次を指定している。

```c
TxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CSUM |
                      ETH_TX_PACKETS_FEATURES_CRCPAD;
TxConfig.CRCPadCtrl = ETH_CRC_PAD_INSERT;
```

これにより、アプリケーションがFCSを手作業で付ける必要はない。

## 4. MAC内部とDMAの関係

### 4.1 中級者向けの内部構成

STM32H753のEthernet peripheralは、大きく次のブロックに分けて考えられる。

```text
RAM
 ↕ descriptor / buffer
DMA
 ↕
MTL（送受信FIFO、キュー制御）
 ↕
MAC（フレーム処理、フィルター、CRCなど）
 ↕
RMII
```

- DMA：RAMとEthernet peripheralの間を転送する。
- MTL：MAC Transaction Layer。MACとDMA間の送受信FIFOやキューを管理する。
- MAC：Ethernetフレームそのものを処理する。

HAL APIではこれらを一体の`ETH` peripheralとして扱うことが多いが、不具合解析では区別すると分かりやすい。

### 4.2 descriptorとは

descriptorは、DMAへバッファ情報を伝える管理票である。概念的には次を保持する。

```text
buffer address
data length
packet status
CPU/DMA ownership
next descriptor information
```

現在はRX/TXそれぞれ4個のdescriptorを使う。複数用意することで、1個のフレームをCPUが処理している間にも別の転送を進められる。

descriptorとpacket bufferは同じものではない。

- descriptor：DMAが転送先や状態を知るための管理情報。
- buffer：実際のEthernetフレーム内容が入るRAM。

### 4.3 送信経路

現在の送信処理は次の順序である。

```text
lwIPがpbufを作る
  ↓
low_level_output()がpbuf chainをTx buffer情報へ変換
  ↓
D-CacheをCleanしてCPUの変更をRAMへ反映
  ↓
HAL_ETH_Transmit()
  ↓
DMAがRAMから読み、MTL、MAC、RMIIへ転送
  ↓
LAN8742AからLANケーブルへ送信
```

pbufが複数につながっている場合、各部分を`ETH_BufferTypeDef`のchainとしてDMA送信設定へ渡す。

### 4.4 受信経路

```text
LAN8742AがRMIIへデータを出す
  ↓
MACがフレームを受信
  ↓
DMAがRX bufferへ格納
  ↓
ETH割り込み
  ↓
HAL_ETH_RxCpltCallback()が受信フラグを立てる
  ↓
メインループがHAL_ETH_ReadData()を呼ぶ
  ↓
D-CacheをInvalidateしてDMAの最新データをCPUから見えるようにする
  ↓
lwIPへpbufを渡す
```

受信割り込みの中ではlwIPを直接実行しない。RTOSなしの`NO_SYS=1`構成では、lwIP処理をメインループへ集約するためである。

### 4.5 D-Cacheが関係する理由

MACではなくDMAがRAMへ直接アクセスするため、Cortex-M7のD-CacheとRAMの内容が食い違う可能性がある。

```text
送信: CPU cache → Clean → RAM → DMA
受信: DMA → RAM → Invalidate → CPU cache
```

descriptor領域はMPUで非キャッシュにし、送受信データは32バイトのcache lineを考慮してClean/Invalidateする。これは「MAC設定」だけでは解決できない、CPUとDMAの共有メモリ設計である。

## 5. PHYとの速度・duplex連携

### 5.1 Auto-negotiationの結果をMACへ反映する

速度とduplexを物理的に決めるのはPHY同士のAuto-negotiationである。しかしSTM32 MACも同じ条件で動作するよう設定しなければならない。

現在の`ethernet_link_check_state()`はLAN8742Aの状態を読み、次のようにMACへ反映する。

```text
PHY: 100 Mbps Full Duplex → MAC: 100M / Full Duplex
PHY: 100 Mbps Half Duplex → MAC: 100M / Half Duplex
PHY:  10 Mbps Full Duplex → MAC:  10M / Full Duplex
PHY:  10 Mbps Half Duplex → MAC:  10M / Half Duplex
```

条件確定後に`HAL_ETH_Start_IT()`でEthernetを割り込みモード開始し、lwIPのnetifをlink upにする。

### 5.2 duplex不一致

PHYとMAC、またはリンク両端でduplexが一致しないと、低負荷では動くように見えても、負荷を上げたときに再送、CRC error、packet lossが増えることがある。Auto-negotiationを使う場合も、その結果をMACへ正しく設定する必要がある。

## 6. RMIIとは

### 6.1 RMIIの目的

RMIIはReduced Media Independent Interfaceの略で、Ethernet MACとPHYを少ない端子数で接続する仕様である。MIIでは送受信データが各4ビット幅だが、RMIIは各2ビット幅に減らし、送受信で共通の50 MHz基準クロックを使う。

RMIIは次の速度に対応する。

- 100 Mbps
- 10 Mbps

RMII自体がTCP/IPを理解するわけではない。MACが作ったフレームを2ビットずつPHYへ渡すデジタルな通路である。

### 6.2 なぜ2ビットで100 Mbpsになるのか

100 Mbps動作では、1クロックごとに2ビットを転送する。

```text
50 MHz × 2 bit = 100 Mbit/s
```

10 Mbps動作でもREF_CLKは50 MHzのままで、同じ2ビット値を複数クロックにわたって扱うことで速度を下げる。したがって、リンク速度が10 MbpsになってもRMII基準クロックを5 MHzへ変更するわけではない。

### 6.3 RMIIとMDC/MDIOは別経路

```text
RMII data path  : TXD、TX_EN、RXD、CRS_DV、REF_CLK
PHY management : MDC、MDIO
```

MDC/MDIOでPHYレジスタが読めても、RMIIの50 MHzクロックやデータ配線が誤っていればpingは通らない。逆にRMII信号が存在しても、MDIOでリンク条件を取得できなければMACを正しく設定できない。

## 7. RMII信号の意味

現在のプロジェクトで使う信号は次のとおりである。方向はSTM32を基準にする。

| STM32端子 | RMII信号 | 方向 | 意味 |
|---|---|---|---|
| PA1 | REF_CLK | 入力 | 送受信共通の連続50 MHz基準クロック |
| PA7 | CRS_DV | 入力 | Carrier Sense / Receive Data Valid |
| PC4 | RXD0 | 入力 | 受信データbit 0 |
| PC5 | RXD1 | 入力 | 受信データbit 1 |
| PG11 | TX_EN | 出力 | 送信中であることをPHYへ示す |
| PG13 | TXD0 | 出力 | 送信データbit 0 |
| PB13 | TXD1 | 出力 | 送信データbit 1 |
| PC1 | MDC | 出力 | PHY管理クロック |
| PA2 | MDIO | 双方向 | PHY管理データ |

### 7.1 REF_CLK

REF_CLKはRMII全体の時間基準である。連続した50 MHzが必要で、停止、周波数違い、大きなジッター、信号品質不良があると送受信できない。

LAN8742Aは構成によって25 MHz crystalからRMII用50 MHzを生成できる。実基板でクロック源と方向を判断するときは、PHYデータシートだけでなくNUCLEOの回路図、strap設定、実装部品を確認する。

### 7.2 TXD[1:0]とTX_EN

STM32 MACがREF_CLKに同期して2ビットずつTXDへ出し、有効期間にTX_ENをアサートする。PHYはこれを10BASE-Tまたは100BASE-TX信号へ変換する。

### 7.3 RXD[1:0]とCRS_DV

PHYが受信したデータを2ビットずつRXDへ出し、受信データが有効な期間をCRS_DVでMACへ伝える。MACはこれをバイト列とEthernetフレームへ復元する。

### 7.4 MDCとMDIO

MDC/MDIOはLAN8742AのBCR、BSR、PHYSCSRなどを読み書きする。現在のコードは`HAL_ETH_SetMDIOClockRange()`でSTM32の動作クロックに合わせたMDC分周を設定し、`HAL_ETH_ReadPHYRegister()`と`HAL_ETH_WritePHYRegister()`でアクセスする。

## 8. 現在のソースコードとの対応

### 8.1 GPIOとクロック

`HAL_ETH_MspInit()`は次を行う。

- ETH1 MAC/TX/RX peripheral clockを有効化する。
- RMII、MDC、MDIO端子を`GPIO_AF11_ETH`に設定する。
- `ETH_IRQn`を有効にする。

端子がGPIOのまま、別Alternate Function、または競合peripheralへ設定されていると、コード上でETH初期化が成功しても信号は端子へ出ない。

### 8.2 MAC/DMA初期化

`low_level_init()`は次を`heth.Init`へ設定する。

```c
heth.Init.MACAddr        = &MACAddr[0];
heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
heth.Init.TxDesc         = DMATxDscrTab;
heth.Init.RxDesc         = DMARxDscrTab;
heth.Init.RxBuffLen      = 1536;
```

`HAL_ETH_Init()`が、この情報を使ってMAC、DMA、RMII interfaceを初期化する。

### 8.3 PHY管理

```text
ETH_PHY_IO_Init()     → MDIO clock range設定
ETH_PHY_IO_ReadReg()  → PHY register読み出し
ETH_PHY_IO_WriteReg() → PHY register書き込み
LAN8742_Init()        → PHY address検出
LAN8742_GetLinkState()→ Link、speed、duplex取得
```

### 8.4 受信割り込み

`HAL_ETH_RxCpltCallback()`は受信フラグを立てるだけである。メインループの`MX_LWIP_Process()`がフラグを消費し、`ethernetif_input()`を呼ぶ。

この設計により、Ethernet受信の応答性を割り込みで確保しながら、lwIPを割り込みコンテキストから切り離している。

## 9. 初心者向けの確認手順

### 段階1：PHYを管理できるか

デバッガで次を確認する。

- `LAN8742_Init()`が成功する。
- `LAN8742.DevAddr`が0～31になる。
- MDIO読み出し値が常に`0xFFFF`や`0x0000`ではない。

ここまで成功すれば、MDC/MDIO、PHY電源、Resetは概ね正常と考えられる。

### 段階2：物理リンクが成立するか

- RJ45 Link LEDが点灯する。
- `LAN8742_GetLinkState()`が100Mまたは10M状態を返す。
- Auto-negotiationが完了する。

ここまで成功しても、RMIIデータとDMAが正常とは限らない。

### 段階3：MAC/DMAまでフレームが届くか

- PCからpingまたはARPを送る。
- `ETH_IRQHandler()`へ入る。
- `HAL_ETH_RxCpltCallback()`へ入る。
- `RxDataAvailable`が1になる。

リンク済みなのに割り込みが来ない場合は、RMII信号、MAC設定、DMA、NVICを確認する。

### 段階4：lwIPまで届くか

- `ethernetif_input()`が呼ばれる。
- `HAL_ETH_ReadData()`からpbufを取得できる。
- WiresharkでボードのARP Replyを確認できる。
- 最後にICMP Echo Replyを確認する。

## 10. 中級者向けの測定と切り分け

### 10.1 オシロスコープ／ロジックアナライザー

| 観測信号 | 分かること |
|---|---|
| REF_CLK | 50 MHz基準クロックの有無、品質 |
| MDC/MDIO | PHY管理アクセスが実際に出ているか |
| CRS_DV/RXD | PCからのフレームがPHYからMACへ来るか |
| TX_EN/TXD | STM32が応答フレームをPHYへ出しているか |

50 MHz信号の測定では、プローブの帯域、ground lead、負荷によって波形を崩さないよう注意する。

### 10.2 Wireshark

| 見える最後のパケット | 主に疑う場所 |
|---|---|
| ARP Requestのみ | ボード受信、ARP処理、送信経路 |
| ARP Replyあり、ping Requestのみ | ICMP、checksum、受信後処理 |
| ping Replyも見えるがPCが失敗扱い | PC設定、重複IP、checksum表示、routing |

### 10.3 エラーカウンターと状態

中級者向けの解析では次も確認する。

- `heth.ErrorCode`
- DMA statusとRBU（Receive Buffer Unavailable）
- RX poolの枯渇
- CRC error、alignment error、runt/giant frame
- MAC management counters
- descriptor ownershipがDMAからCPUへ戻るか

単発pingだけでなく、連続pingやUDP負荷をかけると、buffer不足、cache不整合、duplex不一致が見つかりやすい。

## 11. よくある誤解

### 「リンクLEDが点灯したのでMACもlwIPも正常」

リンクLEDが示すのは主にPHY間の物理リンクである。RMII受信、DMA、cache、lwIP、IP設定までは保証しない。

### 「MDIOでPHYを読めるのでRMIIも正常」

MDC/MDIOとRMII data pathは別である。PHY registerを読めても、REF_CLKやRXD/TXDが誤っていればフレームは通らない。

### 「MACアドレスとIPアドレスはどちらか一方だけでよい」

同一LANの配送にはMACアドレス、ネットワーク層の識別にはIPアドレスを使う。ARPが両者を対応付ける。

### 「DMAを使えばCPUは何もしなくてよい」

DMAは転送を担当するが、descriptorの準備、bufferの寿命、割り込み処理、cache整合性、lwIPへの受け渡しはソフトウェアが管理する。

### 「10 MbpsならREF_CLKは5 MHz」

RMIIでは10 Mbps時も50 MHz基準クロックを使用する。データ表現側で転送速度を調整する。

## 12. 設計・変更時のチェックリスト

### MAC/DMA

- MACアドレスがLAN内で一意か。
- PHY結果とMACのspeed/duplexが一致するか。
- RX/TX descriptor数とbuffer数が負荷に十分か。
- DMAからアクセス可能なRAMか。
- descriptor領域が重複していないか。
- D-CacheのClean/Invalidateと32バイト整列が正しいか。
- ETH割り込みが有効か。

### RMII/PHY

- STM32とPHYのinterface modeが両方RMIIか。
- REF_CLKが連続50 MHzか。
- 信号方向と端子Alternate Functionが正しいか。
- MDC clockがPHY仕様範囲内か。
- PHY Resetとstrap取り込みが正しいか。
- Auto-negotiation結果をMACへ反映しているか。
- 基板配線、電圧、signal integrityに問題がないか。

### ソフトウェア

- `HAL_ETH_Init()`が成功するか。
- PHY addressを検出できるか。
- link up後に`HAL_ETH_Start_IT()`を呼ぶか。
- `MX_LWIP_Process()`が停止せず呼ばれるか。
- ISR内から直接lwIP coreを呼んでいないか。

## 13. 関連資料

- [ST RM0433：STM32H743/753 Reference Manual](https://www.st.com/resource/en/reference_manual/rm0433-stm32h743-753-and-stm32h750-value-line-advanced-arm-based-32-bit-mcus-stmicroelectronics.pdf)
- [STM32H753製品ページ](https://www.st.com/en/microcontrollers-microprocessors/stm32h753xi.html)
- [Microchip LAN8742Aデータシート](https://www.microchip.com/content/dam/mchp/documents/OTH/ProductDocuments/DataSheets/DS_LAN8742_00001989A.pdf)
- [Microchip LAN8742A製品ページ](https://www.microchip.com/en-us/product/LAN8742A)
- [全体像と資料の読み方](./ethernet_documentation_overview.md)
- [LAN8742A解説書](./lan8742a_explanation.md)
- [lwIP設定解説書](./lwip_configuration_explanation.md)
- [Ethernet DMA・D-Cache対応解説書](./stm32h753_ethernet_dma_dcache_explanation.md)
