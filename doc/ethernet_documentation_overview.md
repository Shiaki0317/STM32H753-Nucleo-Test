# STM32H753 Ethernet開発資料：全体像と読み方

## 1. 全体像

今回の開発は、NUCLEO-H753ZIのLAN8742Aを使い、Ethernet DMAで受信し、lwIPでIPv4通信を処理しながら、Cortex-M7のD-Cacheを有効にしても安定して`ping`へ応答できるようにするものである。

```text
PC（ping 192.168.2.10）
  │
  │ LANケーブル上の10BASE-T / 100BASE-TX信号
  ▼
LAN8742A PHY
  │  電気信号とRMII信号を相互変換
  ▼
STM32H753 Ethernet MAC
  │  Ethernetフレームを制御
  ▼
Ethernet DMA ←→ DMA descriptor / Rx・Tx buffer ←→ RAM
  │                                                ▲
  ▼                                                │ D-Cache整合性
HAL ETH → ethernetif.c → lwIP → ARP / IPv4 / ICMP ┘
```

この構成には、目的の異なる4つの設定がある。

| 分野 | 目的 | 代表的な設定・処理 |
|---|---|---|
| LAN8742A | LANケーブルとの物理リンクを作る | RMII、MDC/MDIO、Auto-negotiation |
| ETH MAC/DMA | フレームをRAMと送受信する | descriptor、buffer、ETH割り込み |
| lwIP | ARP、IP、ICMP、TCP/UDPを処理する | IP、netmask、gateway、`NO_SYS` |
| MPU/D-Cache | CPUを高速化しつつDMAとデータを一致させる | 非キャッシュ領域、Clean、Invalidate |

どれか1つだけ正しくても通信は完成しない。物理リンク、DMA転送、プロトコル処理、キャッシュ整合性がすべてつながって、初めてpingが成功する。

## 2. 現在の完成構成

```text
IPv4                 192.168.2.10/24（固定IP）
Gateway              192.168.2.1
MAC address          02:00:00:00:00:01
RTOS                 なし（NO_SYS=1）
受信通知             Ethernet DMA割り込み
lwIP受信処理         メインループ
PHYリンク監視        100 ms周期
D-Cache              有効
DMA descriptor領域   MPUで非キャッシュ
Tx/Rx data            Clean / Invalidateで整合
```

「定期処理」という言葉だけでは、現在の受信方式を正確に表せない。

```text
パケット受信       イベント発生 → DMA割り込み → フラグ設定
lwIPへの受け渡し   メインループがフラグを見て実行
lwIPタイマー       メインループごとに期限を確認
PHYリンク監視      100 ms周期でレジスタ確認
```

## 3. 資料の読み方

初めて読む場合は、次の順序を推奨する。

1. 本書でシステム全体と用語の境界を理解する。
2. [lwIP設定 解説書](./lwip_configuration_explanation.md)でIP通信と実行方式の意味を理解する。
3. [STM32 Ethernet MAC・RMII解説書](./stm32_ethernet_mac_rmii_explanation.md)でMAC、DMA、PHY間の信号を理解する。
4. [LAN8742A 解説書](./lan8742a_explanation.md)でPHY、MDC/MDIO、Auto-negotiationを理解する。
5. [Ethernet DMA・D-Cache対応 解説書](./stm32h753_ethernet_dma_dcache_explanation.md)でDMAとキャッシュの関係を理解する。
6. 実作業では[lwIP設定手順書](./lwip_configuration_procedure.md)と[LAN8742A設定・確認手順書](./lan8742a_setup_procedure.md)を使う。
7. 一連の接続、生成、デバッグ、ping、GitHub反映は[Ethernet DMA・D-Cache対応手順書](./stm32h753_ethernet_dma_dcache_procedure.md)に従う。
8. 実際の変更点を確認するときは[`.ioc`およびソースコード変更内容](./ioc_and_source_changes.md)を参照する。

## 4. 各資料の役割

| 資料 | 読む目的 |
|---|---|
| [lwIP設定手順書](./lwip_configuration_procedure.md) | CubeMXでIP、lwIP、ETHを設定し、ping確認する |
| [lwIP設定 解説書](./lwip_configuration_explanation.md) | `NO_SYS`、netif、pbuf、タイマー、チェックサムの意味を学ぶ |
| [STM32 Ethernet MAC・RMII解説書](./stm32_ethernet_mac_rmii_explanation.md) | MAC、frame、DMA、descriptor、RMII信号と確認方法を学ぶ |
| [LAN8742A設定・確認手順書](./lan8742a_setup_procedure.md) | PHYを初期化し、リンクとAuto-negotiationを確認する |
| [LAN8742A 解説書](./lan8742a_explanation.md) | PHY、RMII、MDC/MDIO、主要レジスタの意味を学ぶ |
| [DMA・D-Cache対応手順書](./stm32h753_ethernet_dma_dcache_procedure.md) | 接続から書き込み、動作確認、GitHub反映まで実行する |
| [DMA・D-Cache対応 解説書](./stm32h753_ethernet_dma_dcache_explanation.md) | 割り込み方式とキャッシュ対策の理由を学ぶ |
| [変更内容一覧](./ioc_and_source_changes.md) | 既定生成との差分と変更理由を確認する |

## 5. 初心者が覚えるべき境界

### リンクが成立するまで

主役はLAN8742A、RMIIクロック、LANケーブル、MDC/MDIOである。この段階ではIPアドレスは関係しない。

### フレームがRAMへ届くまで

主役はSTM32 ETH MAC、DMA、descriptor、buffer、ETH割り込みである。リンクLEDが点灯しても、この部分が誤っていればlwIPへデータは届かない。

### pingへ応答するまで

主役は`ethernetif.c`とlwIPである。ARPでMACアドレスを解決し、IPv4とICMPを処理する。

### D-Cache有効で安定するまで

主役はMPU、メモリ配置、32バイト整列、Clean/Invalidateである。CPUとDMAが同じRAMを別々のタイミングで参照するため、明示的な整合処理が必要になる。

## 6. 確認も下位層から行う

```text
1. 電源・配線・リンクLED
2. PHYアドレスのMDIO読み出し
3. Link、speed、duplex
4. ETH DMA割り込み
5. 受信バッファとD-Cache
6. ARP要求・応答
7. ICMP ping
8. TCP/UDPアプリケーション
```

この順序なら、問題が「物理層」「DMA」「キャッシュ」「lwIP」「PC設定」のどこにあるかを段階的に絞り込める。

## 7. 主要な一次資料

- [lwIP公式：NO_SYSモード](https://www.nongnu.org/lwip/2_1_x/group__lwip__nosys.html)
- [lwIP公式：ネットワークインターフェース](https://www.nongnu.org/lwip/2_1_x/group__netif.html)
- [ST UM1713：STM32CubeでのlwIPアプリケーション開発](https://www.st.com/resource/en/user_manual/um1713-developing-applications-on-stm32cube-with-lwip-tcpip-stack-stmicroelectronics.pdf)
- [Microchip LAN8742A製品ページ](https://www.microchip.com/en-us/product/LAN8742A)
- [Microchip LAN8742Aデータシート](https://www.microchip.com/content/dam/mchp/documents/OTH/ProductDocuments/DataSheets/DS_LAN8742_00001989A.pdf)
- [STMicroelectronics LAN8742 BSP component](https://github.com/STMicroelectronics/stm32-lan8742)
