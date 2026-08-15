# STM32H753 Nucleo Ethernet DMA・D-Cache対応手順書

## 1. 目的

NUCLEO-H753ZI上でlwIP Ethernet通信を動作させ、受信処理をEthernet DMA割り込み通知方式へ変更し、Cortex-M7のD-Cacheを有効にした状態で安定動作を確認するまでの手順を示す。

本書では次の作業を一連の流れとして扱う。

1. PC、ST-LINK、ボード、Ethernetの接続確認
2. STM32CubeIDEでのプロジェクト起動とデバッグ
3. 既定生成版の受信処理の確認
4. `.ioc`によるETH割り込み、D-Cache、MPU設定
5. Ethernet DMA割り込み受信処理とキャッシュメンテナンスの実装
6. CubeMXによるコード再生成
7. ビルド、書き込み、デバッガ確認
8. pingによる通信確認
9. GitHubへの反映

設定差分の詳細は[`.ioc`およびソースコード変更内容](./ioc_and_source_changes.md)を参照する。

## 2. 対象環境

| 項目 | 内容 |
| --- | --- |
| Board | NUCLEO-H753ZI |
| MCU | STM32H753ZITx |
| IDE | STM32CubeIDE 1.19.0 |
| CubeMX | 6.15.0 |
| Firmware Package | STM32Cube FW_H7 V1.13.0 |
| Network stack | lwIP 2.2.1 Cube版、NO_SYS構成 |
| PHY | LAN8742、RMII |
| Debug probe | NUCLEOオンボードST-LINK |
| OpenOCD | 0.12.0 |
| Board IP | `192.168.2.10/24` |
| Gateway | `192.168.2.1` |
| Repository | `Shiaki0317/STM32H753-Nucleo-Test` |

本環境固有のパスは必要に応じて読み替える。

```text
Project:
/home/shidaa/Workspace/Workspace-Micon/stm32h753_Nucleo

STM32CubeIDE:
/opt/st/stm32cubeide_1.19.0/stm32cubeide
```

## 3. ハードウェア接続

### 3.1 接続構成

- NUCLEO-H753ZIのST-LINK USBコネクタをPCへ接続する。
- ボードのEthernetコネクタとPC側NICまたは同一LANのスイッチを接続する。
- ボードとPC側NICを同じ`192.168.2.0/24`ネットワークに設定する。
- PC側アドレスには、ボードと重複しないアドレス、例として`192.168.2.1/24`を使用する。

### 3.2 ST-LINK接続確認

LinuxでUSBデバイスを確認する。

```bash
lsusb
```

続いてOpenOCDからST-LINKおよびSTM32H7ターゲットへ接続する。

```bash
openocd \
  -f interface/stlink.cfg \
  -f target/stm32h7x.cfg \
  -c 'init; targets; shutdown'
```

次の状態であれば接続は正常である。

- ST-LINKが検出される。
- STM32H7ターゲットが表示される。
- `shutdown command invoked`で正常終了する。

### 3.3 Ethernet物理リンク確認

PC側NIC名を確認する。

```bash
ip -brief address
ip route
```

本環境のNIC名が`enp11s0`の場合は、次のコマンドでリンクを確認する。

```bash
ethtool enp11s0
```

確認項目:

- `Link detected: yes`
- PC側に`192.168.2.x/24`が設定されている。
- ボードIPの`192.168.2.10`と重複していない。

## 4. STM32CubeIDEの起動

STM32CubeIDEをワークスペース指定で起動する。

```bash
/opt/st/stm32cubeide_1.19.0/stm32cubeide \
  -data /home/shidaa/Workspace/Workspace-Micon
```

GUIから起動する場合は、STM32CubeIDE Launcherで次をワークスペースとして指定する。

```text
/home/shidaa/Workspace/Workspace-Micon
```

プロジェクトが表示されない場合は、次の手順で既存プロジェクトを読み込む。

1. `File` → `Import...`を選択する。
2. `General` → `Existing Projects into Workspace`を選択する。
3. プロジェクトルートとして`stm32h753_Nucleo`を指定する。
4. `Finish`を選択する。

## 5. 変更前の受信処理確認

既定生成版のNO_SYS構成では、`main()`の無限ループから`MX_LWIP_Process()`が繰り返し呼ばれる。

変更前は`MX_LWIP_Process()`内で次の処理が毎回実行される。

```c
ethernetif_input(&gnetif);
sys_check_timeouts();
Ethernet_Link_Periodic_Handle(&gnetif);
```

また、リンク確立時は次の非割り込みAPIでEthernetを開始する。

```c
HAL_ETH_Start(&heth);
```

したがって変更前は、Ethernet DMA自体は使用しているが、受信済みデータの確認はメインループからのポーリング処理である。

今回の対応では、受信完了をETH割り込みで通知し、通知がある場合だけメインループでlwIP受信処理を行う。

## 6. `.ioc`のEthernet基本設定確認

STM32CubeIDEで`stm32h753_Nucleo.ioc`を開き、次の設定を確認する。

### 6.1 ETH設定

| 項目 | 設定値 |
| --- | --- |
| Media Interface | RMII |
| MAC Address | `02:00:00:00:00:01` |
| Rx descriptor | `0x30040000` |
| Tx descriptor | `0x30040200` |
| Rx buffer | `0x30040400` |

### 6.2 lwIP設定

| 項目 | 設定値 |
| --- | --- |
| DHCP | Disabled |
| IP address | `192.168.2.10` |
| Netmask | `255.255.255.0` |
| Gateway | `192.168.2.1` |
| PHY driver | LAN8742 |

RMIIの各信号が次のピンへ割り当てられていることも確認する。

| Signal | Pin |
| --- | --- |
| ETH_REF_CLK | PA1 |
| ETH_MDIO | PA2 |
| ETH_CRS_DV | PA7 |
| ETH_TXD1 | PB13 |
| ETH_MDC | PC1 |
| ETH_RXD0 | PC4 |
| ETH_RXD1 | PC5 |
| ETH_TX_EN | PG11 |
| ETH_TXD0 | PG13 |

## 7. ETH割り込みの有効化

CubeMX画面で次の操作を行う。

1. `System Core` → `NVIC`を開く。
2. `Ethernet global interrupt`を有効にする。
3. Preemption Priorityを`0`にする。
4. Sub Priorityを`0`にする。

保存後、`.ioc`に次の設定が存在することを確認する。

```ini
NVIC.ETH_IRQn=true\:0\:0\:false\:false\:true\:true\:true\:true
```

## 8. D-Cacheの有効化

CubeMX画面で次の操作を行う。

1. `System Core` → `CORTEX_M7`を開く。
2. CPU D-Cacheを`Enabled`にする。
3. CPU I-Cacheは本対応では`Disabled`のままとする。

`.ioc`の確認値:

```ini
CORTEX_M7.CPU_DCache=Enabled
CORTEX_M7.CPU_ICache=Disabled
```

D-Cacheだけを有効にすると、CPUとEthernet DMAが異なるデータを見る可能性がある。このため次節のMPU設定と送信時のキャッシュクリーン処理を合わせて実施する。

## 9. Ethernet DMA descriptor用MPU設定

`CORTEX_M7`のMPU設定でRegion 1を追加する。

| 項目 | 設定値 |
| --- | --- |
| Region | Region 1 |
| Enable | Enabled |
| Base Address | `0x30040000` |
| Size | `1 KB` |
| Access Permission | Full Access |
| Instruction Access | Disable / Execute Never |
| Shareable | Shareable |
| Cacheable | Non-cacheable |
| Bufferable | Bufferable |

この領域には次のディスクリプタが含まれる。

```text
0x30040000: Rx DMA descriptors
0x30040200: Tx DMA descriptors
0x30040400: Region 1終端、Rx buffer開始
```

`.ioc`には少なくとも次の設定が保存される。

```ini
CORTEX_M7.Enable_S-Cortex_Memory_Protection_Unit_Region1_Settings_S=MPU_REGION_ENABLE
CORTEX_M7.BaseAddress_S-Cortex_Memory_Protection_Unit_Region1_Settings_S=0x30040000
CORTEX_M7.Size_S-Cortex_Memory_Protection_Unit_Region1_Settings_S=MPU_REGION_SIZE_1KB
CORTEX_M7.AccessPermission_S-Cortex_Memory_Protection_Unit_Region1_Settings_S=MPU_REGION_FULL_ACCESS
CORTEX_M7.DisableExec_S-Cortex_Memory_Protection_Unit_Region1_Settings_S=MPU_INSTRUCTION_ACCESS_DISABLE
CORTEX_M7.IsShareable_S-Cortex_Memory_Protection_Unit_Region1_Settings_S=MPU_ACCESS_SHAREABLE
CORTEX_M7.IsBufferable_S-Cortex_Memory_Protection_Unit_Region1_Settings_S=MPU_ACCESS_BUFFERABLE
```

## 10. ソースコード変更

### 10.1 Ethernet割り込みハンドラ

`Core/Inc/stm32h7xx_it.h`へ宣言を追加する。

```c
void ETH_IRQHandler(void);
```

`Core/Src/stm32h7xx_it.c`でEthernet HALハンドルを参照し、HAL IRQハンドラを呼び出す。

```c
extern ETH_HandleTypeDef heth;

void ETH_IRQHandler(void)
{
  HAL_ETH_IRQHandler(&heth);
}
```

### 10.2 ETH NVIC初期化

`LWIP/Target/ethernetif.c`の`HAL_ETH_MspInit()`で割り込みを有効にする。

```c
HAL_NVIC_SetPriority(ETH_IRQn, 0, 0);
HAL_NVIC_EnableIRQ(ETH_IRQn);
```

`HAL_ETH_MspDeInit()`では無効にする。

```c
HAL_NVIC_DisableIRQ(ETH_IRQn);
```

`.ioc`から再生成する場合、これらはETH IRQ設定に従って生成される。再生成後にも存在することを確認する。

### 10.3 Ethernet DMAを割り込みモードで開始

リンク確立時の開始処理を変更する。

```diff
- HAL_ETH_Start(&heth);
+ HAL_ETH_Start_IT(&heth);
```

### 10.4 受信通知フラグ

`LWIP/Target/ethernetif.c`のUSER CODE領域に通知フラグを追加する。

```c
static volatile u8_t RxDataAvailable;
```

受信完了コールバックでフラグをセットする。

```c
void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *handlerEth)
{
  (void)handlerEth;
  RxDataAvailable = 1U;
}
```

Receive Buffer Unavailable発生時も受信処理を再開できるようにする。

```c
void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *handlerEth)
{
  if ((HAL_ETH_GetDMAError(handlerEth) & ETH_DMACSR_RBU) != 0U)
  {
    RxDataAvailable = 1U;
  }
}
```

フラグの読み出しとクリアを不可分に行う関数を追加する。

```c
u8_t ethernetif_rx_pending(void)
{
  uint32_t primask;
  u8_t pending;

  primask = __get_PRIMASK();
  __disable_irq();
  pending = RxDataAvailable;
  RxDataAvailable = 0U;
  if (primask == 0U)
  {
    __enable_irq();
  }

  return pending;
}
```

`LWIP/Target/ethernetif.h`のUSER CODE領域へ宣言を追加する。

```c
u8_t ethernetif_rx_pending(void);
```

### 10.5 lwIP受信処理

`LWIP/App/lwip.c`の`MX_LWIP_Process()`を、通知がある場合だけ受信処理を行う形にする。

```c
if (ethernetif_rx_pending() != 0U)
{
  ethernetif_input(&gnetif);
}

sys_check_timeouts();
Ethernet_Link_Periodic_Handle(&gnetif);
```

受信データ処理は割り込み通知式になるが、次の処理は引き続きメインループの定期処理である。

- lwIP timeout処理
- PHYリンク状態確認
- アプリケーションがメインループから呼ぶその他の処理

割り込み内でlwIP APIを直接実行しないこと。割り込みではフラグだけをセットし、lwIP処理はメインコンテキストで行う。

### 10.6 D-Cache初期化

`Core/Src/main.c`でMPU設定後、HAL初期化前にD-Cacheを有効にする。

```c
MPU_Config();
SCB_EnableDCache();
HAL_Init();
```

`MPU_Config()`にはRegion 1の設定が生成されていることを確認する。

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

### 10.7 送受信バッファのキャッシュメンテナンス

D-Cache有効時は、`low_level_output()`で各pbufセグメントをDMAへ渡す前にキャッシュをクリーンする。

```c
if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
{
  SCB_CleanDCache_by_Addr((uint32_t *)q->payload, q->len);
}
```

この処理はCPUが書き換えた送信データをRAMへ反映し、Ethernet DMAが最新データを参照できるようにする。

受信方向では、`HAL_ETH_RxLinkCallback()`に次のinvalidate処理が存在することを確認する。

```c
if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
{
  SCB_InvalidateDCache_by_Addr((uint32_t *)buff, Length);
}
```

これはCPUのD-Cacheに残る古い内容を無効にし、DMAがRAMへ書いた最新の受信データをCPUに読み込ませるために必要である。

## 11. CubeMXコード再生成

### 11.1 GUIでの再生成

推奨手順:

1. `stm32h753_Nucleo.ioc`を保存する。
2. `Project` → `Generate Code`を実行する。
3. Firmware Packageの確認画面が表示された場合は、STM32Cube FW_H7 V1.13.0を使用する。
4. 生成完了後、Problemsビューに生成エラーがないことを確認する。

### 11.2 再生成後の保持確認

次のコマンドで必要な設定とUSER CODEが残っていることを確認する。

```bash
rg -n \
  'SCB_EnableDCache|MPU_REGION_NUMBER1|HAL_ETH_Start_IT|ETH_IRQHandler|HAL_ETH_RxCpltCallback|ethernetif_rx_pending|SCB_CleanDCache_by_Addr|SCB_InvalidateDCache_by_Addr' \
  Core LWIP stm32h753_Nucleo.ioc
```

次の項目がすべて検索されることを確認する。

- `SCB_EnableDCache()`
- `MPU_REGION_NUMBER1`
- `HAL_ETH_Start_IT()`
- `ETH_IRQHandler()`
- `HAL_ETH_RxCpltCallback()`
- `ethernetif_rx_pending()`
- `SCB_CleanDCache_by_Addr()`
- `SCB_InvalidateDCache_by_Addr()`

`USER CODE BEGIN/END`の外側へ手動追加したコードは再生成で失われる可能性があるため、再生成前後の差分を必ず確認する。

## 12. ビルド

### 12.1 STM32CubeIDEでのビルド

1. Project Explorerで`stm32h753_Nucleo`を選択する。
2. `Project` → `Build Project`を実行する。
3. Consoleにエラーがないことを確認する。

### 12.2 コマンドラインでのビルド

`Debug`ディレクトリでCubeIDE付属ツールチェーンをPATHへ追加してビルドする。

```bash
cd /home/shidaa/Workspace/Workspace-Micon/stm32h753_Nucleo/Debug

env PATH=/opt/st/stm32cubeide_1.19.0/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.linux64_1.0.0.202410170706/tools/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  make -j4 all
```

確認済みビルド結果:

```text
text    101556 bytes
data       168 bytes
bss      36040 bytes
```

## 13. ボードへの書き込み

### 13.1 STM32CubeIDEから書き込む場合

1. プロジェクトをビルドする。
2. `stm32h753_Nucleo.launch`を選択する。
3. `Debug As` → `STM32 C/C++ Application`を実行する。
4. ST-LINK接続後、Flash downloadとVerifyが成功することを確認する。

### 13.2 OpenOCDから書き込む場合

プロジェクトルートで実行する。

```bash
openocd \
  -f interface/stlink.cfg \
  -f target/stm32h7x.cfg \
  -c 'program Debug/stm32h753_Nucleo.elf verify reset exit'
```

成功条件:

- Programming Finished
- Verified OK
- Reset後にエラーなく終了

## 14. デバッガによる確認

### 14.1 STM32CubeIDEでの確認

次のブレークポイントを使用する。

| 確認箇所 | 目的 |
| --- | --- |
| `main()` | リセット後にアプリケーションへ到達すること |
| `MX_LWIP_Init()`直後 | lwIPとETH HAL初期化の完了確認 |
| `HAL_ETH_RxCpltCallback()` | ping受信時にETH割り込みが発生すること |
| `ethernetif_input()` | 通知後にメインループで受信処理すること |
| `HardFault_Handler()` | 異常停止の監視 |
| `Error_Handler()` | HAL初期化エラーの監視 |

ExpressionsまたはMemoryビューで次を確認する。

```c
(SCB->CCR & SCB_CCR_DC_Msk) != 0U
```

値が真であればD-Cacheは有効である。

次の変数も確認する。

```text
gnetif.flags
gnetif.ip_addr
heth.gState
heth.ErrorCode
RxDataAvailable
```

期待結果:

- `main()`へ到達する。
- PHYリンク確立後にnetifのlink/upフラグがセットされる。
- `heth.ErrorCode`に致命的エラーがない。
- ping送信時に`HAL_ETH_RxCpltCallback()`へ到達する。
- 割り込み復帰後に`ethernetif_input()`へ到達する。

### 14.2 OpenOCDでD-Cacheを確認する場合

SCB CCRのDCビットを確認する。

```bash
openocd \
  -f interface/stlink.cfg \
  -f target/stm32h7x.cfg \
  -c init \
  -c halt \
  -c 'mdw 0xE000ED14 1' \
  -c resume \
  -c shutdown
```

`0xE000ED14`のbit 16が1であればD-Cacheは有効である。

### 14.3 MPU Region 1を確認する場合

MPU Region Number RegisterへRegion 1を指定し、RBARとRASRを読む。

```bash
openocd \
  -f interface/stlink.cfg \
  -f target/stm32h7x.cfg \
  -c init \
  -c halt \
  -c 'mww 0xE000ED98 1' \
  -c 'mdw 0xE000ED9C 1' \
  -c 'mdw 0xE000EDA0 1' \
  -c resume \
  -c shutdown
```

RBARが`0x30040000`を示し、RASRでRegion Enableと1 KB相当のサイズ・非キャッシュ属性が設定されていることを確認する。

## 15. ping通信確認

ボードをリセットして数秒待った後、PCから実行する。

```bash
ping -c 6 -W 2 192.168.2.10
```

合格条件:

- 6回送信して6回応答する。
- Packet lossが0%である。
- 実行中にHardFaultまたはError Handlerへ遷移しない。
- デバッガ使用時、ETH受信コールバックと`ethernetif_input()`へ到達する。

ARP状態も合わせて確認できる。

```bash
ip neigh show 192.168.2.10
```

## 16. 最終受入確認

| No. | 確認内容 | 合格条件 |
| --- | --- | --- |
| 1 | ST-LINK接続 | OpenOCDがSTM32H7を認識 |
| 2 | Ethernet PHY | Link detected |
| 3 | `.ioc` ETH IRQ | ETH global interrupt有効 |
| 4 | `.ioc` D-Cache | CPU D-Cache Enabled |
| 5 | `.ioc` MPU | Region 1が`0x30040000`、1 KB、非キャッシュ |
| 6 | コード再生成 | 必須USER CODEが保持される |
| 7 | ビルド | エラー0 |
| 8 | Flash書き込み | Verify成功 |
| 9 | デバッグ | `main()`到達、HardFaultなし |
| 10 | D-Cache | SCB CCR DC bit = 1 |
| 11 | MPU | Region 1のベースと属性が一致 |
| 12 | DMA受信通知 | ETH受信コールバックへ到達 |
| 13 | lwIP受信 | 通知後に`ethernetif_input()`を実行 |
| 14 | ping | 6/6応答、損失0% |

## 17. トラブルシューティング

### 17.1 ST-LINKへ接続できない

- USBケーブルが充電専用でないことを確認する。
- 他のOpenOCD、GDB、STM32CubeIDEデバッグセッションがST-LINKを占有していないか確認する。
- ボード電源LEDを確認する。
- OpenOCDの`interface/stlink.cfg`と`target/stm32h7x.cfg`を使用する。

### 17.2 ビルドで`arm-none-eabi-gcc`が見つからない

- STM32CubeIDE付属GNU Toolsの`bin`をPATHへ追加する。
- STM32CubeIDEからビルドする。
- `Debug/makefile`が再生成されていることを確認する。

### 17.3 pingが応答しない

次の順で確認する。

1. PC側NICが`192.168.2.0/24`に設定されているか。
2. ボードIPが`192.168.2.10`か。
3. LAN8742のリンク状態がLink Upか。
4. `HAL_ETH_Start_IT()`が実行されたか。
5. `ETH_IRQHandler()`へ到達するか。
6. `HAL_ETH_RxCpltCallback()`で`RxDataAvailable`が1になるか。
7. メインループで`ethernetif_rx_pending()`が1を返すか。
8. `ethernetif_input()`へ到達するか。
9. `heth.ErrorCode`とDMAエラーステータスを確認する。

### 17.4 D-Cache有効化後に通信できない

- MPU Region 1が`0x30040000`、1 KB、Non-cacheableか確認する。
- Rx/Tx descriptorのリンカ配置が`.ioc`のアドレスと一致するか確認する。
- TX前に`SCB_CleanDCache_by_Addr()`が実行されるか確認する。
- RX受信後に`SCB_InvalidateDCache_by_Addr()`が実行されるか確認する。
- Rx bufferが32バイト境界にアラインされているか確認する。
- CubeMX再生成でキャッシュメンテナンス処理が消えていないか確認する。

### 17.5 受信処理が継続しない

- `HAL_ETH_ErrorCallback()`でRBUを検出して通知を再設定しているか確認する。
- Rx pool枯渇後にバッファが解放されているか確認する。
- `ethernetif_input()`が受信キューを`NULL`になるまで処理しているか確認する。
- 割り込み優先度が意図した値か確認する。

### 17.6 CubeMX再生成後に変更が消えた

- 手動コードを`USER CODE BEGIN/END`内へ配置する。
- `.ioc`にETH IRQ、D-Cache、MPU設定が保存されているか確認する。
- 再生成前にGit差分またはバックアップを作成する。
- 再生成直後に`rg`コマンドで必須シンボルを確認する。

## 18. GitHub反映手順

### 18.1 差分確認

```bash
git status --short --branch
git diff --check
git diff --stat
```

`Debug/`などの生成物をコミットしない。現在の`.gitignore`では次を除外している。

```text
/Debug/
/.agents/
/.codex/
/.vscode/
```

### 18.2 コミットとPush

変更ファイルを明示してステージする。

```bash
git add stm32h753_Nucleo.ioc Core LWIP doc
git commit -m "Describe the change"
git push -u origin agent/enable-dcache-ethernet-dma
```

Push後はPull Requestの差分を確認し、意図しない生成物やローカル絶対パスが含まれていないことを確認してから`main`へマージする。

## 19. 実施済み結果

今回の実施結果は次のとおり。

| 項目 | 結果 |
| --- | --- |
| STM32CubeIDE起動 | 成功 |
| ST-LINK接続 | 成功 |
| ビルド | 成功 |
| Flash書き込み・Verify | 成功 |
| `main()`到達 | 成功 |
| D-Cache有効状態 | 確認済み |
| MPU Region 1 | 確認済み |
| Ethernet DMA割り込み受信 | 動作確認済み |
| ping | 6回送信、6回応答 |
| Firmware commit | `e0ba300` |
| 差分資料commit | `9283495` |
| 資料PR | `#2` |
