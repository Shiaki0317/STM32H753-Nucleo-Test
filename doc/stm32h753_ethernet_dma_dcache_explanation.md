# STM32H753 Ethernet DMA・D-Cache対応 解説書

## 1. 本書の目的

本書は、NUCLEO-H753ZIでEthernet通信を成立させるために実施した各手順について、単なる操作方法ではなく、次の観点から意味を説明するものである。

- なぜその確認を行うのか
- なぜ`.ioc`を変更するのか
- なぜそのソースコードを変更するのか
- 変更によって処理の責務と実行タイミングがどう変わるのか
- ビルド、デバッグ、ping確認がそれぞれ何を保証するのか

具体的な操作は[Ethernet DMA・D-Cache対応手順書](./stm32h753_ethernet_dma_dcache_procedure.md)、設定値の差分は[`.ioc`およびソースコード変更内容](./ioc_and_source_changes.md)を参照する。

---

# 第I部 全体像

## 2. 今回の対応を一言で表すと

今回の対応は、STM32H753のEthernet通信を次の構成にする作業である。

> Ethernet DMAが受信を行い、受信完了割り込みはメインループへ通知だけを行い、lwIPの実処理はメインコンテキストで実行する。D-Cacheを有効にする一方、DMAとCPUが共有するメモリについてMPU設定とキャッシュメンテナンスを行い、データ整合性を維持する。

重要なのは、単に「DMAを有効にする」「D-Cacheを有効にする」という二つの独立した変更ではない点である。

- Ethernet DMAはCPUとは独立してRAMを読み書きする。
- D-Cacheを有効にすると、CPUが参照する最新データがRAMではなくキャッシュに存在する場合がある。
- DMAはCPUのD-Cacheを参照できない。
- そのため、CPUとDMAの間でメモリ内容が食い違わない設計が必要になる。

今回のMPU設定とキャッシュクリーン処理は、この食い違いを防ぐために存在する。

## 3. システム構成

処理に関係する要素は次のとおりである。

```text
PC
 │ ping / Ethernet frame
 ▼
LAN8742 PHY
 │ RMII
 ▼
STM32H753 Ethernet MAC + 専用DMA
 │ DMA descriptor / Rx buffer / Tx buffer
 ▼
RAM
 ▲                    ▲
 │                    │
CPU Cortex-M7      D-Cache
 │
 ▼
HAL ETH driver
 │
 ▼
ethernetif.c
 │
 ▼
lwIP
 │
 ▼
ICMP応答生成
```

役割は次のように分かれる。

| 要素 | 役割 |
| --- | --- |
| LAN8742 | Ethernetの電気信号とRMII信号を変換するPHY |
| Ethernet MAC | Ethernetフレームの送受信を制御する |
| Ethernet DMA | CPUを介さずRAMとMACの間でデータを転送する |
| DMA descriptor | DMAへバッファの場所、長さ、所有権、状態を伝える管理情報 |
| HAL ETH | STM32のMAC/DMAを操作するドライバ |
| `ethernetif.c` | HAL ETHとlwIPを接続するインターフェース層 |
| lwIP | ARP、IP、ICMP、TCP、UDPなどのプロトコル処理を行う |
| MPU | メモリ領域ごとのアクセス権とキャッシュ属性を定義する |
| D-Cache | CPUからRAMへのアクセスを高速化する |

## 4. 変更前と変更後の違い

### 4.1 変更前

```text
main loop
  └─ MX_LWIP_Process()
      ├─ ethernetif_input()を毎回実行
      ├─ sys_check_timeouts()
      └─ Ethernet_Link_Periodic_Handle()
```

変更前もEthernet HAL内部ではDMAを利用している。しかし、受信済みデータがあるかどうかをメインループから毎回確認していた。

この方式は単純だが、受信データがない場合にも`HAL_ETH_ReadData()`へ到達するため、メインループの実行回数に応じたポーリング処理になる。

### 4.2 変更後

```text
DMA受信完了
  └─ ETH_IRQHandler()
      └─ HAL_ETH_IRQHandler()
          └─ HAL_ETH_RxCpltCallback()
              └─ RxDataAvailable = 1

main loop
  └─ MX_LWIP_Process()
      ├─ 通知ありの場合だけethernetif_input()
      ├─ sys_check_timeouts()
      └─ Ethernet_Link_Periodic_Handle()
```

変更後は、受信完了をハードウェア割り込みで検出する。一方、lwIP処理そのものは割り込み内へ移動していない。

この構成には次の意味がある。

- 受信データがないときの不要な受信確認を減らす。
- 割り込み処理を短く保つ。
- lwIPを従来と同じメインコンテキストで動作させる。
- NO_SYS構成で複雑な排他制御を増やさない。

## 5. 対応全体の流れと各工程の目的

| 工程 | 実施内容 | その工程の意味 |
| --- | --- | --- |
| 1 | ST-LINK接続確認 | ソフトウェア以前に、PCからMCUを制御できることを保証する |
| 2 | Ethernet物理リンク確認 | PHY、ケーブル、PC側NICの問題をファームウェア問題から分離する |
| 3 | ping確認 | EthernetからlwIPまでのエンドツーエンド動作を確認する |
| 4 | CubeIDEデバッグ | MCU内部状態と停止箇所を直接確認する |
| 5 | 既定受信処理の調査 | ポーリングされている処理と定期処理の責務を区別する |
| 6 | ETH IRQ有効化 | DMA受信完了をCPUへ通知する経路を作る |
| 7 | `HAL_ETH_Start_IT()`化 | Ethernet DMAの割り込み通知を実際に開始する |
| 8 | 通知フラグ追加 | ISRとメインループを安全かつ軽量に接続する |
| 9 | D-Cache有効化 | CPUのデータアクセス性能を向上させる |
| 10 | MPU設定 | DMA descriptorのキャッシュ不整合を防止する |
| 11 | TX cache clean | CPUが作った送信内容をDMAから見えるRAMへ反映する |
| 12 | CubeMX再生成 | `.ioc`を設定の正本として生成コードへ反映する |
| 13 | ビルド | ソースと設定がコンパイル・リンク可能なことを保証する |
| 14 | Flash/Verify | 作成したバイナリが正しくボードへ書かれたことを保証する |
| 15 | レジスタ確認 | D-CacheとMPUが実行時にも有効であることを保証する |
| 16 | ping再確認 | すべての変更を組み合わせても通信が成立することを保証する |
| 17 | Git/PR | 変更理由、差分、検証結果を再現可能な履歴として残す |

---

# 第II部 各手順の意味

## 6. 接続確認を最初に行う理由

### 6.1 ST-LINK確認

OpenOCDでST-LINKとターゲットを認識できるか確認する作業は、次の経路を検証している。

```text
PC USB
  → ST-LINK
    → SWD
      → STM32H753 debug port
```

この確認が失敗している状態では、ファームウェアのビルドが成功しても書き込みやデバッグはできない。したがって、ソース解析より前に接続問題を除外する意味がある。

### 6.2 Ethernet物理リンク確認

`ethtool`によるLink検出は、主に次の範囲を確認する。

- Ethernetケーブル
- PC側NIC
- PHY間のオートネゴシエーション
- リンク速度とDuplex

物理リンクがDownの場合、IPアドレス、lwIP、DMA、D-Cacheを調査してもpingは成功しない。階層の低い場所から順に確認することで、調査範囲を限定できる。

### 6.3 PC側IP設定確認

ボードは`192.168.2.10/24`の固定IPで動作する。PC側NICも同じサブネットに存在する必要がある。

```text
PC:    192.168.2.1/24 など
Board: 192.168.2.10/24
```

同一サブネットでなければPCはボード宛てフレームを直接ARP解決せず、別のゲートウェイへ送ろうとする。この確認は、ファームウェアではなく経路設定の問題を除外するために行う。

## 7. STM32CubeIDEとデバッガを使う意味

ping結果だけでは、失敗箇所が次のどこにあるか判別できない。

- PHYリンクが確立していない。
- MAC/DMAが開始されていない。
- ETH割り込みが発生していない。
- HALコールバックが呼ばれていない。
- lwIPへパケットが渡っていない。
- HardFaultやError Handlerで停止している。

デバッガで各境界にブレークポイントを置くことで、パケットが処理経路のどこまで到達したかを確認できる。

| ブレークポイント | 到達した場合に分かること |
| --- | --- |
| `main()` | リセット後にアプリケーションへ入っている |
| `MX_LWIP_Init()`後 | lwIP初期化処理から復帰している |
| `HAL_ETH_RxCpltCallback()` | MAC/DMAとNVICとIRQハンドラが動作している |
| `ethernetif_input()` | メインループが通知を消費している |
| `HardFault_Handler()` | 不正メモリアクセスなどで停止した |
| `Error_Handler()` | HAL初期化または設定処理が失敗した |

## 8. 受信処理が定期処理か確認した意味

この調査では、`MX_LWIP_Process()`内に性質の異なる処理が混在していることを確認した。

| 処理 | 性質 | 変更後 |
| --- | --- | --- |
| `ethernetif_input()` | 受信イベントに応じて必要になる処理 | DMA通知がある場合だけ実行 |
| `sys_check_timeouts()` | プロトコルタイマを進める定期処理 | 毎回実行を維持 |
| `Ethernet_Link_Periodic_Handle()` | PHYリンク状態を確認する定期処理 | 毎回呼び出しを維持し、内部周期で制御 |

すべてを割り込み化してはいけない理由は、lwIPタイマとリンク監視は受信パケットがなくても進める必要があるためである。

受信処理だけをイベント駆動にし、時間依存処理は定期実行として残すことが今回の設計上の要点である。

## 9. `.ioc`から変更する意味

`.ioc`はCubeMXにとってプロジェクト設定の正本である。生成済みCコードだけを変更し、`.ioc`を変更しなかった場合、次回のコード生成で元の設定へ戻る可能性がある。

今回`.ioc`へ保存した主な設定は次のとおりである。

- ETH global interrupt
- CPU D-Cache
- MPU Region 1
- Ethernet descriptorとbufferの配置
- 固定IPとPHY設定

この作業には二つの意味がある。

1. 現在のCコードを正しく生成する。
2. 将来、別の担当者がCubeMXで開いても同じ設定を再現できるようにする。

## 10. ETH割り込みを有効にした意味

Ethernet peripheral内で受信完了イベントが発生しても、NVICでETH IRQが無効ならCPUは割り込み処理へ入らない。

次の設定はCPUへの通知経路を開く。

```c
HAL_NVIC_SetPriority(ETH_IRQn, 0, 0);
HAL_NVIC_EnableIRQ(ETH_IRQn);
```

ただし、NVICを有効にするだけでは不十分である。以下がすべて必要になる。

```text
HAL_ETH_Start_IT()
  + NVIC ETH IRQ enable
  + vector tableのETH_IRQHandler
  + HAL_ETH_IRQHandler(&heth)
  + HAL callback
```

どれか一つが欠けると、受信イベントはアプリケーションの通知フラグまで届かない。

## 11. `HAL_ETH_Start_IT()`へ変更した意味

変更前:

```c
HAL_ETH_Start(&heth);
```

変更後:

```c
HAL_ETH_Start_IT(&heth);
```

両方ともEthernet MAC/DMAを開始するが、`_IT`版は割り込み通知を利用する前提で動作する。

この変更は「通常のDMAから別のDMAへ切り替えた」という意味ではない。STM32H753のEthernet peripheralは専用DMAを持っており、変更前もフレーム転送にはそのDMAを使っている。

今回変えたのは、受信完了を知る方法である。

| 項目 | 変更前 | 変更後 |
| --- | --- | --- |
| フレーム転送 | Ethernet DMA | Ethernet DMA |
| 受信確認 | メインループから確認 | DMA完了割り込みで通知 |
| lwIP処理場所 | メインループ | メインループ |

## 12. IRQハンドラを追加した意味

`ETH_IRQHandler()`は、Cortex-M7の割り込みベクタとHAL ETHドライバを接続する入口である。

```c
extern ETH_HandleTypeDef heth;

void ETH_IRQHandler(void)
{
  HAL_ETH_IRQHandler(&heth);
}
```

それぞれの意味は次のとおり。

| コード | 意味 |
| --- | --- |
| `extern ETH_HandleTypeDef heth` | `ethernetif.c`で管理されるETH状態をIRQ側から参照する |
| `ETH_IRQHandler()` | startup codeのvector tableから呼ばれる割り込み入口 |
| `HAL_ETH_IRQHandler(&heth)` | DMAステータスを解析し、適切なHAL callbackを呼び出す |

IRQハンドラで直接lwIPを呼ばず、HALへ処理を委譲しているのは、DMAフラグのクリアやHAL状態管理をドライバの規約どおりに実行するためである。

## 13. `RxDataAvailable`を追加した意味

```c
static volatile u8_t RxDataAvailable;
```

この変数は、割り込みコンテキストとメインコンテキストの間を接続するイベント通知である。

### 13.1 `static`の意味

変数の公開範囲を`ethernetif.c`内に限定する。受信通知の管理責務がEthernet interface層にあることを明確にし、他のファイルから無秩序に変更されることを防ぐ。

### 13.2 `volatile`の意味

値が通常のプログラムフロー以外、ここでは割り込みによって変更されることをコンパイラへ伝える。これにより、コンパイラが値をレジスタへ固定し、メモリの更新を見落とす最適化を防ぐ。

### 13.3 1 bit相当の通知でよい理由

フラグは受信パケット数を数えるものではなく、「受信キューを確認する必要がある」という状態を表す。

`ethernetif_input()`は次のループで受信済みパケットを空になるまで処理する。

```c
do
{
  p = low_level_input(netif);
  if (p != NULL)
  {
    netif->input(p, netif);
  }
} while (p != NULL);
```

複数回の割り込みが一つの通知へまとめられても、通知を受けた後にキューをdrainするため、割り込み回数とフラグ回数を一致させる必要はない。

## 14. `HAL_ETH_RxCpltCallback()`を追加した意味

```c
void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *handlerEth)
{
  (void)handlerEth;
  RxDataAvailable = 1U;
}
```

このcallbackはDMA受信完了をアプリケーション層へ通知する場所である。

ここで行うのはフラグセットだけであり、パケット解析は行わない。理由は次のとおり。

- 割り込み占有時間を短くする。
- lwIPの処理を割り込みコンテキストへ持ち込まない。
- 他の割り込みの応答遅延を抑える。
- NO_SYS構成の従来の実行文脈を維持する。

`(void)handlerEth;`は、このcallbackでは引数を使用しないことを明示し、未使用引数警告を防ぐ。

## 15. `HAL_ETH_ErrorCallback()`を変更した意味

```c
if ((HAL_ETH_GetDMAError(handlerEth) & ETH_DMACSR_RBU) != 0U)
{
  RxDataAvailable = 1U;
}
```

RBUはReceive Buffer Unavailableを表す。DMAが受信しようとした時点で利用可能な受信バッファがない場合に発生する。

RBU発生時にもメイン側へ通知する意味は、受信処理とバッファ回収を進め、受信経路が停止したままになることを避けることである。

エラーをすべて同じ扱いにせず、RBUの場合だけ通知しているのは、受信キュー処理で改善できる状態に対象を限定するためである。他のDMAエラーは別途`heth.ErrorCode`やDMA statusを確認して原因を調査する必要がある。

## 16. `ethernetif_rx_pending()`を追加した意味

この関数は通知フラグを読み出し、同時にクリアする。

```c
primask = __get_PRIMASK();
__disable_irq();
pending = RxDataAvailable;
RxDataAvailable = 0U;
if (primask == 0U)
{
  __enable_irq();
}
```

割り込みを一時禁止する理由は、次の競合を防ぐためである。

```text
main: フラグを読む
IRQ : フラグを1にする
main: フラグを0にする  ← 新しい通知を消してしまう
```

読み出しとクリアの間だけ割り込みを禁止すると、この操作が一まとまりになる。禁止区間が短いため、割り込み応答への影響も限定的である。

元のPRIMASKを保存しているのは、呼び出し前から割り込み禁止状態だった場合に、関数が勝手に割り込みを有効化しないためである。

## 17. `MX_LWIP_Process()`を変更した意味

変更後:

```c
if (ethernetif_rx_pending() != 0U)
{
  ethernetif_input(&gnetif);
}

sys_check_timeouts();
Ethernet_Link_Periodic_Handle(&gnetif);
```

この変更は、処理を「イベント依存」と「時間依存」に分離している。

### イベント依存

`ethernetif_input()`は受信イベントがあった場合だけ必要になる。そのため、DMA通知で実行を制御する。

### 時間依存

`sys_check_timeouts()`はTCP再送、ARP、DHCPなどのタイマを進める。受信イベントがなくても呼ぶ必要がある。

`Ethernet_Link_Periodic_Handle()`はPHYリンクの変化を検出する。これも受信パケットの有無とは独立している。

この二種類を一緒に条件分岐へ入れると、無通信時にタイマとリンク監視が止まるため、`ethernetif_input()`だけを条件付きにしている。

## 18. D-Cacheを有効にした意味

D-Cacheは、CPUがRAMから読み出したデータやRAMへ書くデータを、より高速なキャッシュへ保持する機能である。

メリット:

- メモリアクセス待ち時間を減らす。
- lwIPやアプリケーション処理のCPU実行性能を向上できる。

一方で、DMAとの間に次の問題が生じる。

### CPUが書いたデータをDMAが読む場合

```text
CPUが送信データを更新
  → 更新内容がD-Cache内だけに存在
  → RAMは古いまま
  → DMAは古いRAM内容を送信
```

### DMAが書いたデータをCPUが読む場合

```text
DMAがRAMへ受信データを書き込む
  → CPUのD-Cacheには古いデータが残る
  → CPUが古いキャッシュ内容を読む可能性
```

この問題をcache coherency、つまりキャッシュ整合性の問題と呼ぶ。

## 19. MPU Region 1を追加した意味

DMA descriptorはCPUとDMAの両方が頻繁に読み書きする管理情報である。ここに古い値が残ると、次のような致命的な問題につながる。

- DMAが新しいバッファアドレスを認識できない。
- CPUがDMA完了状態を認識できない。
- descriptor所有権が食い違う。
- 送受信が停止する。

そこでdescriptor領域をMPUでNon-cacheableにしている。

```text
Region 1 base: 0x30040000
Region size:   1 KB

0x30040000 Rx descriptors
0x30040200 Tx descriptors
0x30040400 Region end / Rx buffer start
```

各属性の意味:

| 属性 | 設定 | 意味 |
| --- | --- | --- |
| Full Access | 有効 | CPU/HALがdescriptorを読み書きできる |
| Execute Never | 有効 | データ領域から命令を実行しない |
| Shareable | 有効 | CPUとDMAで共有する領域として扱う |
| Cacheable | 無効 | descriptorをD-Cacheへ保持しない |
| Bufferable | 有効 | メモリ書き込みのバッファ特性を許可する |

全RAMをNon-cacheableにせずdescriptorの1 KBだけを対象にしているのは、DMA整合性とCPU性能を両立するためである。

## 20. MPU設定後にD-Cacheを有効にする意味

初期化順序は次のとおりである。

```c
MPU_Config();
SCB_EnableDCache();
HAL_Init();
```

MPUを先に設定する理由は、D-Cacheを有効にした瞬間から正しいメモリ属性を適用するためである。

D-Cacheを先に有効化し、その後でdescriptor領域をNon-cacheableへ変更すると、設定前にdescriptorがキャッシュされる余地が生じる。初期化順序そのものが整合性設計の一部である。

## 21. TX cache cleanを追加した意味

```c
if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
{
  SCB_CleanDCache_by_Addr((uint32_t *)q->payload, q->len);
}
```

`Clean`は、キャッシュ上で変更されたデータをRAMへ書き戻す操作である。送信方向ではDMAがRAMを読むため、DMA開始前にCleanが必要になる。

処理を各pbufセグメントへ実施する理由は、lwIPのパケットが複数のpbufを連結したchainとして表現される場合があるためである。一つ目のバッファだけをCleanすると、後続セグメントが古い内容のまま送信される可能性がある。

D-Cache有効状態を確認してから実行しているのは、無効時の不要なキャッシュ操作を避け、現在のCPU設定に応じて安全に動作させるためである。

キャッシュ操作は32 byte cache line境界を意識する必要がある。使用するCMSIS実装の仕様、バッファのalignment、長さの丸めを、ツールチェーンやCMSIS更新時に再確認する。

## 22. Rx bufferを32 byte alignmentにする意味

STM32H753 Cortex-M7のL1 D-Cache lineは32 byteである。受信バッファがcache line途中から始まると、同じcache line内に別データが混在し、invalidateやclean時に関係のないデータまで影響する可能性がある。

現在の`RxBuff_t`は次の形で32 byte境界を考慮している。

```c
uint8_t buff[(ETH_RX_BUFFER_SIZE + 31) & ~31] __ALIGNED(32);
```

これはサイズを32 byte単位へ切り上げ、開始アドレスも32 byte alignmentにするという意味である。

### 22.1 受信時のcache invalidateが持つ意味

現在の`HAL_ETH_RxLinkCallback()`には、受信bufferに対する次の処理が存在する。

```c
if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
{
  SCB_InvalidateDCache_by_Addr((uint32_t *)buff, Length);
}
```

`Invalidate`は、CPUのD-Cacheに残る古いcache lineを無効にし、次回のCPU読み出しでDMAがRAMへ書いた最新の受信データを取得させる操作である。

送信方向ではCPUが書いてDMAが読むため`Clean`、受信方向ではDMAが書いてCPUが読むため`Invalidate`を使用する。

| 方向 | データ作成者 | データ利用者 | 必要な操作 |
| --- | --- | --- | --- |
| TX | CPU | Ethernet DMA | Clean |
| RX | Ethernet DMA | CPU | Invalidate |

この受信invalidate処理はD-Cache有効化前からEthernet interface側に存在していた。今回新規追加したTX cleanと組み合わせることで、送受信の両方向でcache coherencyを維持する。

## 23. CubeMX再生成を行う意味

コード再生成には次の目的がある。

- `.ioc`設定がCコードへ正しく反映されるか確認する。
- 別環境でも同じ生成結果を得られるようにする。
- 手作業だけに依存した設定を減らす。

一方、再生成には手動コード消失のリスクがある。そのため、次の二つを区別する。

| 種別 | 管理方法 |
| --- | --- |
| CubeMXが生成すべき設定 | `.ioc`へ保存する |
| アプリケーション固有処理 | `USER CODE BEGIN/END`内へ置く |

再生成後に必須シンボルを検索する作業は、「生成が成功した」だけでなく「必要な設計が保持された」ことを確認するために行う。

## 24. ビルド確認の意味

ビルド成功は次を確認する。

- C構文が正しい。
- 関数宣言と定義が一致する。
- ETH IRQハンドラが重複していない。
- 必要なHAL/CMSISシンボルがリンクできる。
- リンカスクリプト上のセクションが配置可能である。

ただし、ビルド成功は実機動作を保証しない。アドレスやcache属性が誤っていても、C言語とリンカの規則上正しければビルドは通る。そのため、書き込み、デバッグ、ping確認が続けて必要になる。

## 25. Flash書き込みとVerifyの意味

書き込みはELFの内容をMCU Flashへ反映する作業である。Verifyは、書き込んだFlash内容とビルド成果物が一致するかを読み戻して確認する。

Verify成功により、次の誤りを除外できる。

- 古いファームウェアを実行している。
- 書き込み途中で失敗した。
- ST-LINK接続不良で一部が更新されなかった。

## 26. 実行時レジスタ確認の意味

### 26.1 SCB CCR

`SCB->CCR`のDC bitを確認することで、ソースに`SCB_EnableDCache()`が存在するだけでなく、実行時にD-Cacheが実際に有効かを確認できる。

### 26.2 MPU RNR/RBAR/RASR

MPU Region 1を選択してRBARとRASRを読むことで、次を実行時に確認できる。

- Region 1が有効である。
- base addressが`0x30040000`である。
- sizeが1 KBである。
- cache属性が意図どおりである。

生成コードの目視確認と実行時レジスタ確認を両方行うことで、「コードはあるが実行されていない」という問題を除外する。

## 27. ping確認が保証する範囲

ping応答が返るまでには次の処理がすべて成功する必要がある。

```text
PCがARPまたはICMPを送信
  → PHY受信
  → MAC受信
  → DMAがRAMへ転送
  → ETH IRQ
  → HAL callback
  → メインループ通知消費
  → ethernetif_input
  → lwIP ICMP処理
  → 応答pbuf生成
  → cache clean
  → DMA送信
  → MAC/PHY送信
  → PCが応答受信
```

したがってping 6/6成功は、単なるIP設定だけでなく、今回変更した受信割り込み経路、lwIP処理、送信DMA、基本的なcache coherencyが組み合わさって動作したことを示す。

ただし、pingは短時間・小容量の確認である。製品レベルでは次の追加試験が望ましい。

- 長時間連続ping
- 大きなパケットと連続トラフィック
- TCP/UDP負荷試験
- リンク抜き差し
- Rx pool枯渇試験
- RBU発生後の復帰
- 複数pbuf chainの送信
- D-Cache有効状態での長時間安定性

## 28. Gitへ差分を保存した意味

組み込みプロジェクトでは、`.ioc`、自動生成コード、USER CODE、リンカスクリプトが相互に依存する。単一ファイルだけでは設定を再現できない。

Gitへ保存する意味は次のとおり。

- `.ioc`と生成コードの対応を残す。
- 変更理由と検証結果をレビューできる。
- CubeMX再生成前後を比較できる。
- 問題発生時に既知の動作版へ戻せる。
- `Debug/`など再生成可能な成果物を除外できる。

Draft PRを使用したのは、`main`へ即時反映せず、差分と検証内容を確認できる状態にするためである。

---

# 第III部 ファイル別の変更理由

## 29. ファイル別まとめ

| ファイル | 変更 | なぜ必要か | 設計上の意味 |
| --- | --- | --- | --- |
| `stm32h753_Nucleo.ioc` | ETH IRQ有効 | CPUへ受信完了を通知するため | 自動生成の正本 |
| `stm32h753_Nucleo.ioc` | D-Cache有効 | CPU性能向上 | 実行性能とDMA整合性を両立する起点 |
| `stm32h753_Nucleo.ioc` | MPU Region 1 | descriptorを非キャッシュ化 | CPU/DMA間の管理情報を常にRAMで共有 |
| `Core/Inc/stm32h7xx_it.h` | IRQ宣言 | 他の生成コードと宣言を共有 | 割り込みAPIの公開 |
| `Core/Src/stm32h7xx_it.c` | ETH IRQ処理 | vectorとHALを接続 | ハードウェアイベントをHALへ渡す境界 |
| `Core/Src/main.c` | MPU設定 | descriptor属性を設定 | cache coherencyの基盤 |
| `Core/Src/main.c` | D-Cache有効化 | CPU性能向上 | MPU設定後にcacheを開始 |
| `LWIP/Target/ethernetif.c` | NVIC設定 | IRQをCPUで受ける | peripheral初期化と割り込みのライフサイクルを一致 |
| `LWIP/Target/ethernetif.c` | `HAL_ETH_Start_IT()` | DMA割り込み通知開始 | ポーリングからイベント通知へ変更 |
| `LWIP/Target/ethernetif.c` | Rx callback | ISRからmainへ通知 | ISRを短くする |
| `LWIP/Target/ethernetif.c` | Error callback | RBU時に処理を促す | 一時的なbuffer不足からの復帰を助ける |
| `LWIP/Target/ethernetif.c` | atomic flag consume | 通知競合防止 | ISR/main間の最小限の同期 |
| `LWIP/Target/ethernetif.c` | TX cache clean | DMAへ最新データを見せる | D-Cache有効時の送信整合性 |
| `LWIP/Target/ethernetif.c` | RX cache invalidateを確認 | CPUへ最新受信データを見せる | D-Cache有効時の受信整合性 |
| `LWIP/Target/ethernetif.h` | pending関数宣言 | lwIP App層から利用 | interface層の内部フラグを直接公開しない |
| `LWIP/App/lwip.c` | 条件付きinput | 不要な受信ポーリングを減らす | イベント処理と定期処理を分離 |

## 30. ヘッダ関数を用意し、フラグを直接公開しなかった理由

`RxDataAvailable`を`extern`で公開せず、`ethernetif_rx_pending()`を通して利用している。

これには次の意味がある。

- フラグの読み出しとクリア方法を一か所へ集約する。
- atomic操作を呼び出し側へ意識させない。
- 将来、フラグからカウンタやイベント構造へ変更してもApp側への影響を小さくする。
- Ethernet interface層の内部実装を隠蔽する。

## 31. 割り込み内でパケットを処理しなかった理由

割り込み内で`ethernetif_input()`やlwIPを直接呼ぶ設計も形式上は可能だが、今回採用していない。

理由:

- パケット処理時間がパケット内容により変動する。
- TCP/IP処理が長いと他の割り込みを遅延させる。
- lwIPの呼び出しコンテキストが変わる。
- 将来処理が増えたときにISR負荷を制御しにくい。
- NO_SYS構成の既存設計を大きく変えずに済む。

今回のフラグ方式は、割り込みをイベント検出だけに限定するdeferred processingの構成である。

## 32. boolean通知方式の意味と制約

今回の`RxDataAvailable`はboolean相当である。これは単純で軽量だが、次の前提に依存する。

- 通知後に`ethernetif_input()`が受信キューを空まで処理する。
- メインループが十分な頻度で回る。
- 受信負荷が処理能力を恒常的に上回らない。

高負荷用途でメインループの遅延が大きい場合は、次の設計も検討する。

- 通知カウンタ
- RTOS semaphore
- task notification
- 専用Ethernet input task

ただし、NO_SYS構成で今回の規模なら、boolean通知とqueue drainの組み合わせは実装が小さく、割り込みとメインの境界も明確である。

## 33. 自動生成コードとUSER CODEの責務

変更は次のように管理する。

### `.ioc`へ持たせる内容

- ETH IRQ有効化
- D-Cache設定
- MPU Region設定
- peripheral pinとclock
- Ethernet memory address
- lwIP基本設定

### USER CODEへ持たせる内容

- `RxDataAvailable`
- HAL callbackの通知処理
- atomicな通知消費関数
- `MX_LWIP_Process()`の条件分岐
- TX cache cleanの補完

この分担により、ハードウェア構成はCubeMXで再現し、アプリケーション固有の動作はUSER CODEとして保持する。

---

# 第IV部 設計上の注意点とまとめ

## 34. この設計で守るべき不変条件

次の条件が崩れると、ビルドが成功しても通信が不安定になる可能性がある。

1. Rx/Tx descriptorのアドレスとMPU Region 1の範囲が一致すること。
2. descriptor領域がNon-cacheableであること。
3. D-CacheはMPU設定後に有効化すること。
4. DMA送信前に送信bufferをCleanすること。
5. DMA受信後、CPUが読む前に受信bufferをInvalidateすること。
6. Rx bufferはcache lineを考慮してalignmentすること。
7. ETH IRQから`HAL_ETH_IRQHandler()`へ処理が渡ること。
8. ISRでは通知だけを行うこと。
9. メインループで受信キューを空まで処理すること。
10. lwIP timeoutとリンク監視を受信通知の条件内へ入れないこと。
11. CubeMX再生成後にUSER CODEとcache処理を再確認すること。

## 35. 各確認結果の読み方

| 結果 | 分かること | まだ保証しないこと |
| --- | --- | --- |
| OpenOCD接続成功 | SWD経路が正常 | アプリケーション動作 |
| Build成功 | コンパイル・リンクが正常 | 実機のメモリ属性 |
| Flash Verify成功 | ELFとFlash内容が一致 | リセット後の正常動作 |
| `main()`到達 | 起動処理が進んでいる | Ethernet通信成立 |
| ETH callback到達 | IRQ/DMA通知経路が動作 | lwIP応答送信 |
| D-Cache bit確認 | cacheが実際に有効 | cache coherency全体 |
| MPU register確認 | descriptor属性が実際に設定 | すべてのbuffer処理 |
| ping成功 | 基本送受信経路が成立 | 長時間・高負荷安定性 |

複数の確認を組み合わせることで、結果の信頼性を高めている。

## 36. 今回の変更が持つ技術的な意味

今回の対応は、受信処理を単純に高速化しただけではない。次の設計原則を適用している。

### 関心の分離

- IRQはイベント検出を担当する。
- HALはhardware status処理を担当する。
- `ethernetif`はHALとlwIPの橋渡しを担当する。
- lwIPはprotocol処理を担当する。
- main loopはdeferred processingと定期処理を担当する。

### 設定の再現性

ハードウェア設定を`.ioc`へ保存し、再生成可能にしている。

### 性能と正しさの両立

D-CacheによるCPU性能を得ながら、DMA descriptorの非キャッシュ化とTX cleanによってデータ整合性を維持している。

### 段階的な検証

物理接続、debug接続、build、flash、runtime register、network communicationを別々に確認し、問題発生時に原因の階層を特定できるようにしている。

## 37. 最終まとめ

今回実行した各手順には、次の一貫した目的がある。

1. 低い階層から順に正常性を確認し、原因範囲を限定する。
2. Ethernet DMA受信完了を割り込みで確実に検出する。
3. 重いlwIP処理は割り込み外で安全に実行する。
4. 定期処理とイベント処理を分離する。
5. D-Cache有効時もCPUとDMAのメモリ整合性を保つ。
6. `.ioc`とUSER CODEの役割を分け、再生成可能にする。
7. ビルドだけでなく実機レジスタとpingで動作を確認する。
8. Gitと文書で変更理由と検証結果を再現可能にする。

ソースコードの変更は個別の対処ではなく、以下の一つの処理連鎖を成立させるためのものである。

```text
受信イベント
  → DMA
  → IRQ
  → HAL callback
  → 軽量な通知
  → main loop
  → lwIP
  → cache整合性を保ったDMA送信
```

この処理連鎖とメモリ整合性の両方が成立して初めて、D-Cacheを有効にしたSTM32H753上でEthernet DMA通信を安定して動作させることができる。
