# STM32H753 Nucleo: `.ioc`およびソースコード変更内容

## 1. 目的

STM32CubeMXで作成した既定生成版を基準として、GitHubへPushした版で実施した以下の変更を整理する。

- Ethernet受信処理を、毎回のポーリングからETH割り込み通知を使う処理へ変更
- Ethernet DMAを割り込みモードで開始
- Cortex-M7のD-Cacheを有効化
- Ethernet DMAディスクリプタ用MPU領域を追加
- D-Cache有効時のEthernet送信データにキャッシュクリーン処理を追加

対象コミットは `e0ba300`、対象ブランチは `agent/enable-dcache-ethernet-dma` である。

## 2. 比較条件

比較対象は次のとおり。

| 区分 | 内容 |
| --- | --- |
| 比較元 | 作業開始時のSTM32CubeMX既定生成状態 |
| 比較先 | GitHubへPushした `stm32h753_Nucleo.ioc` および関連ソース |
| CubeMX | 6.15.0 |
| Firmware Package | STM32Cube FW_H7 V1.13.0 |
| MCU / Board | STM32H753ZITx / NUCLEO-H753ZI |

注意点として、比較元の`.ioc`そのものはGit履歴に保存されていない。差分は、作業時の変更記録とD-Cacheコード生成直前のバックアップから復元した。以下の主要差分は実際に適用した変更内容と一致している。

## 3. `.ioc`ファイルの差分

### 3.1 差分概要

| 設定項目 | 既定生成版 | Push版 | 目的 |
| --- | --- | --- | --- |
| Cortex-M7 D-Cache | `Disabled` | `Enabled` | CPUのデータアクセス性能を向上 |
| ETH global interrupt | 設定なし | 有効、優先度0 | DMA受信完了を割り込みで通知 |
| MPU Region 1 | 設定なし | 有効 | ETH DMAディスクリプタを非キャッシュ領域に配置 |
| MPU Region 1 base | 設定なし | `0x30040000` | Rx/Txディスクリプタ領域の先頭 |
| MPU Region 1 size | 設定なし | `1 KB` | Rxディスクリプタ、Txディスクリプタを包含 |
| Access permission | 設定なし | Full access | CPU/HALからディスクリプタを更新可能にする |
| Execute permission | 設定なし | Execute never | データ領域からの命令実行を禁止 |
| Shareable | 設定なし | Shareable | CPUとDMA間で共有するメモリとして設定 |
| Cacheable | 設定なし | Non-cacheable | DMAとCPUのディスクリプタ不整合を防止 |
| Bufferable | 設定なし | Bufferable | データ領域の書き込み特性を指定 |

### 3.2 D-Cache設定

既定生成版:

```ini
CORTEX_M7.CPU_DCache=Disabled
```

Push版:

```ini
CORTEX_M7.CPU_DCache=Enabled
```

この設定により、CubeMX生成後の`main()`で`SCB_EnableDCache()`が呼ばれる。

### 3.3 ETH割り込み設定

既定生成版には`NVIC.ETH_IRQn`行がなかった。Push版では次の設定を追加した。

```ini
NVIC.ETH_IRQn=true\:0\:0\:false\:false\:true\:true\:true\:true
```

これによりETH global interruptを有効化し、プリエンプション優先度とサブ優先度を0に設定している。

### 3.4 Ethernet DMAディスクリプタ用MPU領域

Push版で追加した主な設定は次のとおり。

```ini
CORTEX_M7.Enable_S-Cortex_Memory_Protection_Unit_Region1_Settings_S=MPU_REGION_ENABLE
CORTEX_M7.BaseAddress_S-Cortex_Memory_Protection_Unit_Region1_Settings_S=0x30040000
CORTEX_M7.Size_S-Cortex_Memory_Protection_Unit_Region1_Settings_S=MPU_REGION_SIZE_1KB
CORTEX_M7.AccessPermission_S-Cortex_Memory_Protection_Unit_Region1_Settings_S=MPU_REGION_FULL_ACCESS
CORTEX_M7.DisableExec_S-Cortex_Memory_Protection_Unit_Region1_Settings_S=MPU_INSTRUCTION_ACCESS_DISABLE
CORTEX_M7.IsShareable_S-Cortex_Memory_Protection_Unit_Region1_Settings_S=MPU_ACCESS_SHAREABLE
CORTEX_M7.IsBufferable_S-Cortex_Memory_Protection_Unit_Region1_Settings_S=MPU_ACCESS_BUFFERABLE
```

`CORTEX_M7.IPParameters`にも上記Region 1の各項目が追加されている。これはCubeMXが設定項目を保存・再生成するための管理情報である。

Ethernetメモリ配置との対応は次のとおり。

| 用途 | アドレス | MPU Region 1との関係 |
| --- | --- | --- |
| Rx DMA descriptor | `0x30040000` | 領域内 |
| Tx DMA descriptor | `0x30040200` | 領域内 |
| Rx buffer | `0x30040400` | 1 KB領域の直後 |

既定生成版から存在していたEthernetのRMIIピン、MACアドレス、固定IPアドレス、Rx/Txディスクリプタアドレス自体は変更していない。

## 4. ソースコードの変更内容

### 4.1 変更ファイル一覧

| ファイル | 変更内容 | 種別 |
| --- | --- | --- |
| `Core/Src/main.c` | D-Cache有効化、ETH descriptor用MPU Region 1追加 | CubeMX設定反映 + 補完 |
| `Core/Inc/stm32h7xx_it.h` | `ETH_IRQHandler()`宣言追加 | ETH IRQ設定反映 |
| `Core/Src/stm32h7xx_it.c` | `heth`参照とETH IRQハンドラ追加 | ETH IRQ設定反映 |
| `LWIP/Target/ethernetif.c` | 割り込み開始、受信通知、TXキャッシュクリーンなど | 生成コード + USER CODE |
| `LWIP/Target/ethernetif.h` | `ethernetif_rx_pending()`宣言追加 | USER CODE |
| `LWIP/App/lwip.c` | 受信通知がある場合だけ`ethernetif_input()`を実行 | USER CODE |

### 4.2 D-Cacheの有効化

`MPU_Config()`の後、HAL初期化より前にD-Cacheを有効化した。

```c
MPU_Config();
SCB_EnableDCache();
HAL_Init();
```

MPU設定を先に適用することで、Ethernet DMAディスクリプタ領域を非キャッシュ属性にしてからD-Cacheを有効にしている。

### 4.3 MPU Region 1の追加

`MPU_Config()`に次の設定を追加した。

```c
MPU_InitStruct.Number = MPU_REGION_NUMBER1;
MPU_InitStruct.BaseAddress = 0x30040000;
MPU_InitStruct.Size = MPU_REGION_SIZE_1KB;
MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE;
HAL_MPU_ConfigRegion(&MPU_InitStruct);
```

目的は、CPUがキャッシュ上の古いディスクリプタを参照したり、DMAがCPUキャッシュ内にだけ存在する更新を参照できなかったりする不整合を防ぐことである。

### 4.4 Ethernet割り込みハンドラ

`Core/Src/stm32h7xx_it.c`でEthernet HALハンドルを参照し、IRQをHALへ渡す処理を追加した。

```c
extern ETH_HandleTypeDef heth;

void ETH_IRQHandler(void)
{
  HAL_ETH_IRQHandler(&heth);
}
```

対応する関数宣言を`Core/Inc/stm32h7xx_it.h`へ追加した。

### 4.5 ETH NVICの初期化・終了処理

`HAL_ETH_MspInit()`でETH IRQを有効化し、`HAL_ETH_MspDeInit()`で無効化するよう変更した。

```c
HAL_NVIC_SetPriority(ETH_IRQn, 0, 0);
HAL_NVIC_EnableIRQ(ETH_IRQn);
```

```c
HAL_NVIC_DisableIRQ(ETH_IRQn);
```

### 4.6 Ethernet DMAの割り込みモード開始

リンク確立時の開始APIを変更した。

```diff
- HAL_ETH_Start(&heth);
+ HAL_ETH_Start_IT(&heth);
```

Ethernet HALは既定生成版でも内部でDMAディスクリプタを使用する。今回の変更は、汎用DMAチャネルを新規追加したものではなく、Ethernet DMAの受信完了通知を割り込みモードに変更したものである。

### 4.7 受信完了通知

割り込みコンテキストからメインループへ通知するフラグを追加した。

```c
static volatile u8_t RxDataAvailable;
```

受信完了時はHALコールバックでフラグをセットする。

```c
void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *handlerEth)
{
  (void)handlerEth;
  RxDataAvailable = 1U;
}
```

DMAのReceive Buffer Unavailable（RBU）発生時も受信処理を再実行できるようにした。

```c
void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *handlerEth)
{
  if ((HAL_ETH_GetDMAError(handlerEth) & ETH_DMACSR_RBU) != 0U)
  {
    RxDataAvailable = 1U;
  }
}
```

`ethernetif_rx_pending()`は割り込みを一時的に禁止し、通知フラグの読み出しとクリアを不可分に実行する。

### 4.8 lwIP受信処理の変更

既定生成版では、`MX_LWIP_Process()`が呼ばれるたびに`ethernetif_input()`を実行していた。

Push版では、DMA受信通知がある場合にだけ呼び出す。

```c
if (ethernetif_rx_pending() != 0U)
{
  ethernetif_input(&gnetif);
}
```

`ethernetif_input()`は1パケットだけでなく、`HAL_ETH_ReadData()`が`NULL`を返すまで受信済みパケットをまとめて処理する。

なお、`MX_LWIP_Process()`自体はメインループから継続して呼ばれる。したがって、以下は引き続き定期処理である。

- `sys_check_timeouts()`によるlwIPタイマ処理
- `Ethernet_Link_Periodic_Handle()`によるリンク状態確認

一方、実データの取り出し処理はETH DMA割り込みの通知がある場合に限定される。

### 4.9 D-Cache有効時の送信処理

送信pbufの各セグメントをEthernet DMAへ渡す前に、CPUキャッシュの内容をメモリへ書き戻す処理を追加した。

```c
if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
{
  SCB_CleanDCache_by_Addr((uint32_t *)q->payload, q->len);
}
```

この処理がない場合、CPUが更新した送信データがD-Cache内に残り、DMAが古いRAM内容を送信する可能性がある。

## 5. 処理フローの変更

既定生成版:

```text
main loop
  -> MX_LWIP_Process()
      -> ethernetif_input()を毎回実行
      -> lwIP timeout処理
      -> link定期確認
```

Push版:

```text
Ethernet DMA受信完了
  -> ETH_IRQHandler()
  -> HAL_ETH_IRQHandler()
  -> HAL_ETH_RxCpltCallback()
  -> RxDataAvailable = 1

main loop
  -> MX_LWIP_Process()
      -> RxDataAvailableが1の場合のみethernetif_input()
      -> lwIP timeout処理
      -> link定期確認
```

割り込み内ではlwIP処理を直接実行せず、フラグ通知だけを行う。これにより割り込み処理時間を短くし、lwIPの実処理を従来どおりメインコンテキストで実行する。

## 6. 自動生成時の注意点

- ETH IRQ、D-Cache、MPU Region 1は`.ioc`に保存されているため、CubeMX再生成の対象になる。
- `USER CODE BEGIN/END`内の受信通知処理は通常の再生成で保持される。
- `ethernetif.c`のTXキャッシュクリーン処理も保持対象として管理する必要がある。CubeMXやFirmware Packageの更新後は、生成テンプレートとの差分を再確認する。
- MPU領域の開始アドレスとリンカスクリプト上のETH descriptor配置は必ず一致させる。
- D-Cache設定だけを有効にし、MPUまたはキャッシュメンテナンスを省略すると、CPUとDMA間でデータ不整合が発生する可能性がある。

## 7. 動作確認結果

Push前に実施した確認結果は次のとおり。

| 確認項目 | 結果 |
| --- | --- |
| STM32CubeIDE生成Makefileによるビルド | 成功 |
| ELFサイズ | text 101,556 bytes / data 168 bytes / bss 36,040 bytes |
| ST-LINKによる書き込み・Verify | 成功 |
| デバッガで`main()`到達 | 成功 |
| D-Cache有効状態 | 確認済み |
| MPU Region 1設定 | 確認済み |
| Ethernet ping | 6回送信、6回応答 |

## 8. GitHub反映情報

| 項目 | 内容 |
| --- | --- |
| Repository | `Shiaki0317/STM32H753-Nucleo-Test` |
| Branch | `agent/enable-dcache-ethernet-dma` |
| Commit | `e0ba300` |
| Pull Request | `#1`（Draft） |
