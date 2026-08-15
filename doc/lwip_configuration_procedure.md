# STM32H753でlwIPを設定する手順書

## 1. 全体像

### 1.1 この手順書の目的

この手順書は、NUCLEO-H753ZI上のEthernetをSTM32CubeMX／STM32CubeIDEで設定し、lwIPを使ってPCから`ping`できる状態にするまでを説明する。

このプロジェクトの通信経路は次のとおりである。

```text
PCのping
  ↓ LANケーブル
LAN8742A（PHY：電気信号とデジタル信号を変換）
  ↓ RMII
STM32H753内蔵Ethernet MAC／DMA
  ↓ ethernetif.c
lwIP（ARP、IPv4、ICMPなどのプロトコル処理）
  ↓
アプリケーション
```

役割を一言で分けると、LAN8742Aは「LANケーブル側」、STM32のMAC/DMAは「フレーム転送」、lwIPは「IP通信の規則」を担当する。

### 1.2 現在のプロジェクト設定

| 項目 | 現在値 |
|---|---|
| ボード | NUCLEO-H753ZI |
| PHY | LAN8742A（CubeMX上のコンポーネント名は`LAN8742`） |
| MAC-PHY接続 | RMII |
| IPv4 | `192.168.2.10`（固定） |
| ネットマスク | `255.255.255.0` |
| ゲートウェイ | `192.168.2.1` |
| MACアドレス | `02:00:00:00:00:01` |
| DHCP | 無効 |
| RTOS | なし（`NO_SYS=1`） |
| 受信 | ETH DMA割り込みで通知し、メインループで処理 |
| リンク監視 | 100 ms周期 |
| D-Cache | 有効 |

## 2. 事前準備

1. NUCLEO-H753ZIをUSBでPCへ接続する。
2. ボードのEthernetコネクタとPCまたはスイッチをLANケーブルで接続する。
3. PC側Ethernetを、例として`192.168.2.20/24`に設定する。
4. PCとボードで同じIPを使っていないことを確認する。
5. STM32CubeMXまたはSTM32CubeIDEのDevice Configuration Toolで`.ioc`を開く。

直結試験ではゲートウェイは実際には使わない。`192.168.2.10`と`192.168.2.20`が同じ`/24`ネットワーク内にあるため、ARPで相手のMACアドレスを調べて直接通信する。

## 3. CubeMXでの設定手順

### 3.1 EthernetをRMIIに設定する

1. **Connectivity > ETH**を開く。
2. Media Interfaceを**RMII**にする。
3. 次の信号が割り当てられていることを確認する。

| 信号 | 現在の端子 | 意味 |
|---|---|---|
| ETH_MDC | PC1 | PHY管理クロック |
| ETH_MDIO | PA2 | PHYレジスタの読み書き |
| ETH_REF_CLK | PA1 | RMIIの50 MHz基準クロック |
| ETH_CRS_DV | PA7 | 受信データ有効 |
| ETH_RXD0/1 | PC4/PC5 | 受信データ |
| ETH_TX_EN | PG11 | 送信データ有効 |
| ETH_TXD0/1 | PG13/PB13 | 送信データ |

端子はボード配線に合わせる必要がある。別ボードへ移植するときは、この表をそのまま流用せず、回路図を確認する。

### 3.2 MACアドレスとDMAメモリを設定する

ETHのParameter Settingsで次を設定する。

```text
MAC address       02:00:00:00:00:01
Rx descriptor     0x30040000
Tx descriptor     0x30040200
Rx buffer         0x30040400
```

`02`から始まるMACアドレスはローカル管理用として扱えるが、同じネットワーク上で重複させてはいけない。複数台を接続する場合は末尾などを個体ごとに変えるか、一意な生成方式を用意する。

DMA記述子と受信バッファは、Ethernet DMAからアクセスできるRAMへ配置する。本プロジェクトではD2 SRAMの`0x3004xxxx`領域を使用する。

### 3.3 ETH割り込みを有効にする

1. ETHのNVIC Settingsを開く。
2. **Ethernet global interrupt**を有効にする。
3. コード生成後、`HAL_NVIC_EnableIRQ(ETH_IRQn)`が生成されていることを確認する。

この割り込みは主にMAC/DMAの完了通知であり、LANケーブルの抜き差しを検出するPHY割り込みとは別物である。本プロジェクトのリンク状態は100 ms周期で読み取る。

### 3.4 lwIPを有効にする

1. **Middleware and Software Packs > LwIP**を有効にする。
2. PHY Driverに**LAN8742**を選ぶ。
3. RTOSを使わない構成にする。
4. IPv4を次のように設定する。

```text
DHCP             Disabled
IP Address       192.168.2.10
Netmask          255.255.255.0
Gateway          192.168.2.1
```

`.ioc`では`192.168.002.010`のように3桁表記になる場合があるが、自動生成コードでは`192, 168, 2, 10`となる。

### 3.5 チェックサムを設定する

本プロジェクトはEthernet MACのチェックサム機能を使用する。

```c
#define CHECKSUM_BY_HARDWARE 1
#define CHECKSUM_GEN_IP      0
#define CHECKSUM_GEN_UDP     0
#define CHECKSUM_GEN_TCP     0
#define CHECKSUM_CHECK_IP    0
#define CHECKSUM_CHECK_UDP   0
#define CHECKSUM_CHECK_TCP   0
```

これは「チェックサム検査を省略する」という意味ではなく、lwIPのソフトウェア計算を無効にし、MACハードウェアへ担当を移す設定である。ハードウェア側の送受信設定と対になっていなければならない。

### 3.6 D-CacheとMPUを設定する

1. **System Core > CORTEX_M7**でD-Cacheを有効にする。
2. Ethernet DMA記述子領域`0x30040000`から1 KiBを、MPUで非キャッシュ領域にする。
3. 受送信データは32バイトのキャッシュラインを考慮して整列させる。
4. 送信前はClean、受信後はInvalidateを実施する。

詳細は`stm32h753_ethernet_dma_dcache_procedure.md`を参照する。

### 3.7 コードを生成する

1. Project ManagerでツールチェーンをSTM32CubeIDEにする。
2. **Generate Code**を実行する。
3. USER CODE領域が保持されたことを差分で確認する。
4. 特に次のファイルを確認する。

```text
stm32h753_Nucleo.ioc
Core/Src/main.c
Core/Inc/stm32h7xx_hal_conf.h
LWIP/App/lwip.c
LWIP/Target/lwipopts.h
LWIP/Target/ethernetif.c
Drivers/BSP/Components/lan8742/lan8742.c
```

## 4. 自動生成後に必要な処理

### 4.1 初期化は1回だけ呼ぶ

周辺機器初期化後に次を1回呼ぶ。

```c
MX_LWIP_Init();
```

この中で`lwip_init()`、IPアドレス設定、`netif_add()`、既定インターフェース登録、PHY初期化が行われる。

### 4.2 メインループで継続処理する

RTOSなしのため、`while (1)`から次を繰り返し呼ぶ。

```c
while (1)
{
  MX_LWIP_Process();
}
```

長い待ち時間や無限待ちをメインループへ入れると、受信、TCP再送、ARPなどが遅れる。重い処理は小分けにする。

### 4.3 受信通知をメインループへ渡す

本プロジェクトは次の流れになっている。

```text
フレーム受信
 → Ethernet DMAがRAMへ格納
 → ETH割り込み
 → HAL_ETH_RxCpltCallback()がRxDataAvailable=1
 → メインループのMX_LWIP_Process()
 → ethernetif_input()
 → lwIPがARP/IP/ICMPを処理
```

`NO_SYS=1`では、ISR内から直接lwIPのコア処理を呼ばない。割り込みは短い通知だけにし、lwIPへの入力はメインループの同一コンテキストで行う。

## 5. ビルド、書き込み、確認

### 5.1 ビルドと書き込み

1. STM32CubeIDEでProject > Build Projectを実行する。
2. エラーが0件であることを確認する。
3. Debugを開始してプログラムを書き込む。
4. `main()`および`MX_LWIP_Init()`の後まで実行する。
5. Resumeしてボードを動作させる。

### 5.2 デバッガで見る値

| 変数 | 期待内容 |
|---|---|
| `LAN8742.DevAddr` | 0～31の検出済みPHYアドレス |
| `gnetif.flags` | interface/link状態を含むフラグ |
| `heth.gState` | HAL ETHが異常状態でない |
| `heth.ErrorCode` | 通常はエラーなし |
| `RxDataAvailable` | 受信割り込み後に1、消費後に0 |

`LAN8742_GetLinkState()`が`LAN8742_STATUS_100MBITS_FULLDUPLEX`などを返せば、PHYのリンクと自動ネゴシエーションを確認できる。

### 5.3 PCからpingする

Linuxの例：

```bash
ip -brief address
ping -c 4 -W 2 192.168.2.10
```

成功時は応答数、遅延、パケット損失0%を確認する。最初の1回はARP解決のため少し遅くなることがある。

## 6. 問題があるときの確認順序

下位層から順に確認すると原因を絞りやすい。

1. ボード電源、LANケーブル、コネクタLED
2. RMII 50 MHzクロックと端子設定
3. MDC/MDIOでPHYアドレスを検出できるか
4. `LAN8742_GetLinkState()`がリンクアップか
5. ETH DMA割り込みが発生するか
6. DMA記述子、バッファ、MPU、D-Cache整合性
7. `MX_LWIP_Process()`が継続して呼ばれるか
8. PCとボードのIP、ネットマスク、ファイアウォール
9. WiresharkでARP要求と応答が見えるか

### 症状別の目安

| 症状 | 主に疑う場所 |
|---|---|
| リンクLEDが点灯しない | ケーブル、PHY電源/Reset、RMIIクロック |
| PHYアドレスを検出できない | MDC/MDIO、クロック、PHY Reset、ストラップ |
| リンクアップするがARP応答なし | DMA、割り込み、キャッシュ、`MX_LWIP_Process()` |
| ARP応答はあるがping応答なし | IP設定、チェックサム、ICMP設定 |
| D-Cache無効時だけ動く | MPU、整列、Clean/Invalidate範囲 |
| ときどき受信停止 | RX pool枯渇、RBU、処理遅延、記述子管理 |

## 7. 完了条件

- ビルドエラーがない。
- PHYアドレスを検出できる。
- リンク速度とduplexを取得できる。
- `MX_LWIP_Process()`が止まらず実行される。
- PCから`192.168.2.10`へping応答がある。
- D-Cache有効のまま複数回の再起動と連続pingで安定する。

## 8. 関連資料

- [lwIP公式：NO_SYSモード](https://www.nongnu.org/lwip/2_1_x/group__lwip__nosys.html)
- [lwIP公式：ネットワークインターフェース](https://www.nongnu.org/lwip/2_1_x/group__netif.html)
- [ST UM1713：STM32CubeでのlwIPアプリケーション開発](https://www.st.com/resource/en/user_manual/um1713-developing-applications-on-stm32cube-with-lwip-tcpip-stack-stmicroelectronics.pdf)
- `lwip_configuration_explanation.md`
- `lan8742a_setup_procedure.md`
- `stm32h753_ethernet_dma_dcache_procedure.md`
