# NUCLEO-H753ZIのLAN8742A設定・確認手順書

## 1. 全体像

### 1.1 この手順書の目的

LAN8742AをSTM32H753のEthernet PHYとして初期化し、リンク速度・duplexを取得して、lwIPによる通信へつなぐ手順を説明する。

```text
STM32H753                                       LAN8742A              RJ45
┌────────────────────┐                         ┌──────────────┐       ┌─────┐
│ lwIP               │                         │              │       │     │
│ ethernetif.c       │                         │ PHY          │       │ LAN │
│ ETH MAC/DMA        │── RMIIデータ信号 ──────│              │───────│     │
│                    │── MDC/MDIO管理信号 ────│              │       │     │
└────────────────────┘                         └──────────────┘       └─────┘
```

- RMII：実際のEthernetフレームを送受信する信号群。
- MDC/MDIO：PHYの状態確認や設定に使う低速の管理インターフェース。
- LAN8742A：10BASE-T/100BASE-TX対応PHY。IPアドレスは持たない。

CubeMXとSTのBSPではコンポーネント名やファイル名が`LAN8742`になっているが、本書では搭載デバイスをLAN8742Aと呼ぶ。

## 2. ハードウェアの事前確認

NUCLEO-H753ZIのオンボードEthernetを使う場合、通常はRMII信号を外部配線する必要はない。ただしジャンパ変更や別基板への移植時は、必ずボード回路図を確認する。

確認項目：

1. ボードとPHYに電源が供給されている。
2. PHY Resetが解除されている。
3. STM32とLAN8742AがRMIIで配線されている。
4. RMII用50 MHz基準クロックが入力されている。
5. MDC/MDIOが正しく配線されている。
6. RJ45、マグネティクス、LANケーブルに異常がない。
7. PHYアドレスを決めるストラップ状態が正しい。

リンクLEDは物理リンクの有力な手掛かりだが、点灯だけではIP通信の成功を保証しない。

## 3. CubeMX設定

### 3.1 ETHをRMIIにする

1. `.ioc`を開く。
2. **Connectivity > ETH**を選ぶ。
3. Media Interfaceを`RMII`にする。
4. 競合端子がないことを確認する。

現在の信号割り当て：

```text
PC1  ETH_MDC       PA1  ETH_REF_CLK
PA2  ETH_MDIO      PA7  ETH_CRS_DV
PC4  ETH_RXD0      PC5  ETH_RXD1
PG11 ETH_TX_EN     PG13 ETH_TXD0
PB13 ETH_TXD1
```

### 3.2 LAN8742コンポーネントを選ぶ

1. LwIPを有効にする。
2. Platform SettingsまたはPHY Driverで`LAN8742`を選択する。
3. コード生成後、次が存在することを確認する。

```text
Drivers/BSP/Components/lan8742/lan8742.c
Drivers/BSP/Components/lan8742/lan8742.h
```

### 3.3 ETH割り込みを有効にする

NVIC SettingsでEthernet global interruptを有効にする。これはDMA受信完了などの通知に使う。LAN8742AのPHY割り込み端子を使う設定とは別である。

### 3.4 コードを生成する

Generate Code後、次の差分を確認する。

- RMII端子とクロックが意図どおりか。
- PHY driverがLAN8742のままか。
- USER CODE領域のDMA受信通知とキャッシュ処理が保持されたか。
- IPアドレスやDMA配置が不意に初期値へ戻っていないか。

## 4. ソフトウェア初期化の流れ

### 4.1 HAL ETHを初期化する

`ethernetif.c`の`low_level_init()`が次を設定する。

```text
MAC address     02:00:00:00:00:01
Media interface RMII
Rx descriptors  DMARxDscrTab
Tx descriptors  DMATxDscrTab
Rx buffer size  1536
```

その後、`HAL_ETH_Init()`でSTM32内蔵MAC/DMAを初期化する。ここはLAN8742Aそのものの初期化ではない。

### 4.2 PHY用I/O関数を登録する

STのLAN8742 driverは特定MCUへ直接依存しない。STM32 HALを呼ぶ関数群を、次のコンテキストとして渡す。

```c
lan8742_IOCtx_t LAN8742_IOCtx = {
  ETH_PHY_IO_Init,
  ETH_PHY_IO_DeInit,
  ETH_PHY_IO_WriteReg,
  ETH_PHY_IO_ReadReg,
  ETH_PHY_IO_GetTick
};

LAN8742_RegisterBusIO(&LAN8742, &LAN8742_IOCtx);
```

### 4.3 PHYを初期化する

```c
if (LAN8742_Init(&LAN8742) != LAN8742_STATUS_OK)
{
  netif_set_link_down(netif);
  netif_set_down(netif);
  return;
}
```

現在のdriverはMDIOアドレス0～31を調べ、Special Modes RegisterのPHYアドレスフィールドと一致するデバイスを探す。成功すると`LAN8742.DevAddr`へ検出アドレスが入る。

### 4.4 リンク状態を反映する

`LAN8742_GetLinkState()`で次のいずれかを得る。

```text
LINK_DOWN
AUTONEGO_NOTDONE
100MBITS_FULLDUPLEX
100MBITS_HALFDUPLEX
10MBITS_FULLDUPLEX
10MBITS_HALFDUPLEX
```

リンクが成立したら、結果に合わせてSTM32 MACの速度とduplexを設定し、`HAL_ETH_Start_IT()`でMAC/DMAを開始する。その後にlwIPの`netif`をlink upへする。

## 5. 動作確認手順

### 5.1 最初の確認

1. LANケーブルを接続する。
2. ボードをリセットする。
3. RJ45のリンクLEDを確認する。
4. `LAN8742_Init()`の戻り値を確認する。
5. `LAN8742.DevAddr`が0～31であることを確認する。
6. `LAN8742_GetLinkState()`の戻り値を確認する。

### 5.2 デバッガでのブレークポイント

次の順に置くと、どの層まで進んだか判断しやすい。

1. `ETH_PHY_IO_Init()`
2. `LAN8742_Init()`直後
3. `ethernet_link_check_state()`
4. `HAL_ETH_Start_IT()`
5. `HAL_ETH_RxCpltCallback()`

期待結果：

- MDIOクロック範囲設定が成功する。
- PHYアドレス走査が成功する。
- ケーブル接続後に100Mまたは10Mの状態になる。
- MAC設定がPHYのspeed/duplexに一致する。
- PCからARP/pingを送ると受信コールバックへ入る。

### 5.3 PC側の確認

```bash
ip -brief address
ethtool <PC側Ethernetインターフェース名>
ping -c 4 -W 2 192.168.2.10
```

`ethtool`ではPC側リンクの検出、速度、duplexを確認する。ボードが100 Mbps Full Duplexを示すなら、通常は対向側も対応する状態になっている必要がある。

## 6. LANケーブル抜き差し確認

本プロジェクトは100 msごとに`ethernet_link_check_state()`を呼ぶ。

1. pingを継続して送る。
2. ケーブルを抜く。
3. `netif_is_link_up()`がfalseになり、ETHが停止することを確認する。
4. ケーブルを再接続する。
5. 自動ネゴシエーション完了後、MACのspeed/duplexが設定されることを確認する。
6. ping応答が再開することを確認する。

## 7. トラブルシューティング

### 7.1 PHYアドレスが見つからない

確認順：

1. PHY電源とReset。
2. MDC/MDIO端子のAlternate Function。
3. `HAL_ETH_SetMDIOClockRange()`が呼ばれているか。
4. ETH周辺クロックが有効か。
5. PHYアドレスのストラップ。
6. MDIO読み取り値が常に`0x0000`または`0xFFFF`でないか。

### 7.2 PHYは見つかるがリンクしない

確認順：

1. LANケーブルと対向機器。
2. RMII 50 MHzクロック。
3. 自動ネゴシエーションが有効か。
4. BSRのLink Status。
5. PHYSCSRのAuto-negotiation Doneと速度状態。
6. RJ45マグネティクスや基板配線。

### 7.3 リンクするがpingできない

この段階ではPHYより上を疑う。

1. STM32 MACのspeed/duplexがPHY結果と一致しているか。
2. ETH DMA割り込みが有効か。
3. DMA記述子と受信バッファが正しいRAMにあるか。
4. D-CacheのClean/Invalidateがあるか。
5. lwIPのIP設定とメインループ処理。
6. PC側IPとファイアウォール。

### 7.4 自動ネゴシエーションが終わらない

起動直後やケーブル挿入直後は一時的に`AUTONEGO_NOTDONE`でも異常とは限らない。継続する場合はケーブル品質、対向ポート設定、広告能力、RMIIクロックを確認する。片側だけを固定速度にするとduplex不一致が起こることがあるため、評価時は原則として両側Autoを推奨する。

## 8. 完了条件

- `LAN8742_Init()`が成功する。
- PHYアドレスを検出できる。
- ケーブル接続時にlink upになる。
- 10/100 Mbpsおよびduplex状態を取得できる。
- STM32 MACへ同じ通信条件が設定される。
- ケーブル抜き差し後に復帰する。
- D-Cache有効状態でping応答が安定する。

## 9. 関連資料

- [Microchip LAN8742A製品ページ](https://www.microchip.com/en-us/product/LAN8742A)
- [Microchip LAN8742Aデータシート](https://www.microchip.com/content/dam/mchp/documents/OTH/ProductDocuments/DataSheets/DS_LAN8742_00001989A.pdf)
- [STMicroelectronics LAN8742 BSP component](https://github.com/STMicroelectronics/stm32-lan8742)
- `lan8742a_explanation.md`
- `lwip_configuration_procedure.md`
- `stm32h753_ethernet_dma_dcache_procedure.md`
