# LAN8742A 初心者向け解説書

## 1. 全体像

### 1.1 LAN8742Aは何をする部品か

LAN8742Aは10BASE-T/100BASE-TX Ethernet PHYである。PHYはPhysical Layerの略で、STM32内部のデジタルなEthernet信号と、LANケーブルを流れる物理信号を相互変換する。

```text
IPアドレス、ping、TCP/UDP        → lwIPの担当
Ethernetフレーム、MACアドレス   → STM32 ETH MACの担当
RAMとの高速転送                 → STM32 ETH DMAの担当
RMIIとLANケーブル信号の変換     → LAN8742A PHYの担当
```

したがって、LAN8742AへIPアドレスを設定するわけではない。IPアドレスはlwIPのネットワークインターフェースへ設定する。

### 1.2 通信経路

送信時：

```text
lwIP → pbuf → STM32 DMA → STM32 MAC → RMII → LAN8742A → RJ45 → LAN
```

受信時：

```text
LAN → RJ45 → LAN8742A → RMII → STM32 MAC → DMA → pbuf → lwIP
```

管理時：

```text
STM32 HAL → MDC/MDIO → LAN8742A内部レジスタ
```

## 2. MACとPHYの違い

| 項目 | MAC | PHY |
|---|---|---|
| 本プロジェクトの部品 | STM32H753内部 | LAN8742A |
| 主な役割 | フレーム、MACアドレス、CRC、DMA連携 | 電気信号、リンク、速度、duplex |
| CPUとの接続 | MCU内部バス | RMII、MDC/MDIO |
| 代表的な状態 | DMA完了、送受信エラー | Link Down、100M Full Duplex |

通信が失敗したとき、「リンクLEDがつかない」はPHY側、「リンク済みだがARPが返らない」はMAC/DMA/lwIP側の可能性が高い。この境界を意識すると切り分けが速い。

## 3. RMIIの意味

RMII（Reduced Media Independent Interface）はMACとPHYを接続するインターフェースである。MIIより信号線を減らし、10/100 Mbps Ethernetを扱える。

### 3.1 データ信号

| 信号 | 方向（STM32基準） | 意味 |
|---|---|---|
| REF_CLK | 入力 | 50 MHzの共通基準 |
| CRS_DV | 入力 | 受信データが有効 |
| RXD0/1 | 入力 | 2ビット幅の受信データ |
| TX_EN | 出力 | 送信データが有効 |
| TXD0/1 | 出力 | 2ビット幅の送信データ |

50 MHzクロックがなければ、MDC/MDIOでPHYレジスタを読める場合でも、RMIIデータ通信は成立しないことがある。

### 3.2 管理信号

| 信号 | 意味 |
|---|---|
| MDC | STM32が出す管理クロック |
| MDIO | 双方向の管理データ |

MDC/MDIOは、リンクの有無、速度、duplex、Auto-negotiation状態などをレジスタから読むために使う。実際のpingパケットはMDC/MDIOを通らずRMIIを通る。

## 4. PHYアドレスとストラップ

MDIOバスではPHYを5ビットのアドレスで識別するため、範囲は0～31である。PHYのアドレスや起動モードの一部は、Reset解除時の端子状態（ストラップ）で決まる。

本プロジェクトのST driverはアドレスを固定値と決めつけず、0～31を走査する。

```text
各アドレスのSpecial Modes Registerを読む
  ↓
レジスタ内のPHY Addressと読み出し先アドレスが一致するか
  ↓
一致したアドレスをLAN8742.DevAddrへ保存
```

読み取りがすべて`0xFFFF`なら、MDIOがプルアップされたまま、PHYが応答していない、端子設定が違うなどを疑う。すべて`0x0000`でも電源、Reset、クロック、配線を確認する。

## 5. Auto-negotiationの意味

Auto-negotiationは、接続した2台が互いの対応能力を知らせ、使用する速度とduplexを決める仕組みである。LAN8742Aは10 Mbpsと100 Mbps、half/full duplexに対応する。

一般的な優先順位は能力の高い組み合わせであり、両側が対応すれば100 Mbps Full Duplexになる。

- 100 Mbps / Full Duplex
- 100 Mbps / Half Duplex
- 10 Mbps / Full Duplex
- 10 Mbps / Half Duplex

Full Duplexは送受信を同時にできる。Half Duplexは同時に行わず、衝突を考慮する古い方式である。

PHYが決めたspeed/duplexとSTM32 MACの設定は一致させる必要がある。本プロジェクトでは`ethernet_link_check_state()`が結果をMACへ反映する。

### Auto-MDIX

LAN8742AはAuto-MDIXを備え、送受信ペアの組み合わせを自動判定できる。そのため一般的なストレート／クロスケーブルの違いを意識する場面は少ない。ただし断線、品質不良、マグネティクス不良を補う機能ではない。

## 6. 主要レジスタの意味

| レジスタ | 略称 | 主な用途 |
|---|---|---|
| Basic Control Register | BCR | Reset、速度、duplex、Auto-negotiation、Power Down |
| Basic Status Register | BSR | Link Status、Auto-negotiation完了、対応能力 |
| PHY Identifier 1/2 | PHYI1R/PHYI2R | ベンダー、モデル、リビジョン識別 |
| Auto-negotiation Advertisement | ANAR | 自分が相手へ通知する能力 |
| Link Partner Ability | ANLPAR | 相手が通知した能力 |
| Special Modes Register | SMR | PHYアドレスなど |
| PHY Special Control/Status | PHYSCSR | Auto-negotiation結果、速度、duplex |
| Interrupt Mask/Source | IMR/ISFR | PHYイベント割り込みの設定・状態 |

BSRのLink Statusはラッチ動作を考慮する必要があるため、ST driverの`LAN8742_GetLinkState()`はBSRを2回読む。アプリ側が独自に1回だけ読んでdriverと異なる結果になる場合は、この仕様を確認する。

## 7. ST LAN8742 driverの構造

### 7.1 なぜI/O関数を登録するのか

driverはSTM32H7専用ではなく、MDIOアクセス方法を外から受け取る構造になっている。

```c
typedef struct
{
  Init;
  DeInit;
  WriteReg;
  ReadReg;
  GetTick;
} lan8742_IOCtx_t;
```

この設計により、LAN8742Aのレジスタ解釈は共通driverに任せ、STM32H753固有のレジスタ読み書きだけを`ethernetif.c`側で提供できる。

### 7.2 現在の呼び出し関係

```text
LAN8742_Init()
  └─ ETH_PHY_IO_Init()
       └─ HAL_ETH_SetMDIOClockRange()

LAN8742_GetLinkState()
  └─ ETH_PHY_IO_ReadReg()
       └─ HAL_ETH_ReadPHYRegister()

LAN8742_SetLinkState()/StartAutoNego()
  └─ ETH_PHY_IO_WriteReg()
       └─ HAL_ETH_WritePHYRegister()
```

### 7.3 driverの状態値

| 戻り値 | 意味 | 対応 |
|---|---|---|
| `READ_ERROR` | MDIO読み出し失敗 | クロック、端子、PHY応答を確認 |
| `WRITE_ERROR` | MDIO書き込み失敗 | 同上 |
| `ADDRESS_ERROR` | 0～31でPHY未検出 | Reset、電源、ストラップを確認 |
| `LINK_DOWN` | 物理リンクなし | ケーブル、対向機器を確認 |
| `AUTONEGO_NOTDONE` | 条件決定中 | 少し待ち、継続なら対向設定を確認 |
| `100/10 MBITS ...` | リンク条件確定 | MACへ同じ条件を設定 |

## 8. リンク監視と受信割り込みの違い

このプロジェクトには2種類の監視がある。

### 8.1 PHYリンク監視

100 msごとにMDC/MDIOで`LAN8742_GetLinkState()`を呼ぶ。ケーブルの抜き差し、Auto-negotiation完了、速度・duplexを確認する定期処理である。

### 8.2 Ethernet DMA受信通知

フレームが届いたときにSTM32のETH割り込みが発生するイベント処理である。`HAL_ETH_RxCpltCallback()`がメインループへ通知する。

LAN8742A自体にも割り込み機能はあるが、現在のリンク監視はPHY割り込みではなく100 msポーリングである。ETH global interruptを有効にしただけで、PHYのリンク割り込みを使っていることにはならない。

## 9. Link upからpingまでの状態遷移

```text
電源投入
  ↓
STM32 MAC/DMA初期化
  ↓
MDC/MDIO初期化、PHYアドレス検出
  ↓
LANケーブル接続
  ↓
Auto-negotiation
  ↓
速度・duplex確定
  ↓
STM32 MACへ同じ条件を設定
  ↓
HAL_ETH_Start_IT()、netif link up
  ↓
ARP受信 → lwIP処理 → ARP応答
  ↓
ICMP Echo Request → Echo Reply
```

このため、`Link up`はping成功の必要条件だが十分条件ではない。Link up以降のDMA、キャッシュ、lwIP、IP設定も正常である必要がある。

## 10. 不具合の層別切り分け

| 到達点 | 分かること | 次に疑う場所 |
|---|---|---|
| PHY IDを読める | 電源、MDC/MDIOは概ね正常 | RMIIクロック、リンク条件 |
| Link upになる | ケーブルと物理リンクは成立 | MAC速度/duplex、RMIIデータ |
| RX割り込みが来る | MAC/DMAまでフレーム到達 | バッファ、キャッシュ、lwIP入力 |
| ARP応答が返る | Ethernet、ARP、IP設定が概ね正常 | ICMP/チェックサム |
| pingが返る | 基本的な双方向IPv4通信が成立 | 長時間安定性、上位アプリ |

PHYレジスタだけを追い続けても、DMAキャッシュ不整合は直らない。逆にIP設定を変えても、リンクLEDがつかない物理層障害は直らない。最後に成功した層を基準に、1つ上を調べる。

## 11. 安全に設定を変更する考え方

- 通常はAuto-negotiationを使い、PHYとMACの条件を自動で一致させる。
- 固定速度にする場合は対向機器も含めて設計し、duplex不一致を避ける。
- PHYアドレスを固定する場合は基板ストラップとdriver設定を一致させる。
- Reset直後はストラップ取り込みやAuto-negotiationの時間を考慮する。
- レジスタの予約ビットはデータシートどおりに扱う。
- LAN8742Aと似た型名のPHYへ置換するときは、同じRMIIでもレジスタ互換とは限らない。

## 12. 関連資料

- [Microchip LAN8742A製品ページ](https://www.microchip.com/en-us/product/LAN8742A)
- [Microchip LAN8742Aデータシート](https://www.microchip.com/content/dam/mchp/documents/OTH/ProductDocuments/DataSheets/DS_LAN8742_00001989A.pdf)
- [STMicroelectronics LAN8742 BSP component](https://github.com/STMicroelectronics/stm32-lan8742)
- `lan8742a_setup_procedure.md`
- `lwip_configuration_explanation.md`
- `stm32h753_ethernet_dma_dcache_explanation.md`
