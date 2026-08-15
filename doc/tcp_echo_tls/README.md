# TCP Echo TLS 実装・確認手順

## 全体像

本ブランチは、既存の認証付きTCP EchoサーバーをTLS 1.2で保護する。構成は次のとおり。

1. CubeMXでMbedTLS ServerとRNGを有効化する。
2. STM32のハードウェアRNGをMbedTLSの乱数源にする。
3. lwIP Raw APIとMbedTLSのBIOコールバックを接続する。
4. TLSハンドシェイク完了後、`AUTH <token>\r\n` を検証する。
5. 認証成功後の平文をEchoし、TCP上には暗号化済みTLSレコードだけを流す。

待受アドレスは `192.168.2.10`、待受ポートは `4433`、既定トークンは `stm32h753` である。

## CubeMX設定

`.ioc` ではMbedTLS Serverを有効、Clientを無効とし、乱数源に `HW_RNG` を指定した。CubeMX生成により `MBEDTLS/`、`Middlewares/Third_Party/mbedTLS/`、RNG HALドライバー、初期化コードが追加される。

TLSの証明書フライトを保持できるよう、次のLwIP値も設定した。

| 設定 | 値 | 意味 |
| --- | ---: | --- |
| `MEM_SIZE` | 16 KiB | TCPセグメントなどに使うLwIPヒープ |
| `TCP_MSS` | 1460 | Ethernet MTU 1500に対応するTCPデータ長 |
| `TCP_SND_BUF` | 5840 | 4 MSS分のTCP送信バッファ |
| `TCP_SND_QUEUELEN` | 16 | 送信待ちセグメント数 |
| `TCP_WND` | 5840 | TCP受信ウィンドウ |

newlib用ヒープは `0x10000`、スタック予約は `0x2000` に拡張した。MbedTLS内部の動的確保と暗号処理用スタックに必要である。

## 実装上の要点

- lwIPはRaw APIの非ブロッキング方式を維持している。
- 暗号化受信pbufをMbedTLSの受信BIOから順に消費する。
- 暗号化送信は `tcp_write(..., TCP_WRITE_FLAG_COPY)` と `tcp_output()` へ接続する。
- TLS接続はRAM使用量を限定するため同時1接続としている。
- 接続管理構造体はLwIPヒープを圧迫しないよう静的領域へ配置している。
- D-Cache有効時はEthernet DMA送信前にCache Cleanを行う。
- CubeMX同梱のECテスト証明書と秘密鍵を使用する。これは開発確認専用であり、製品では固有証明書と安全な鍵保管へ置き換える。
- 64 MHzの無最適化Debugビルドでは公開鍵演算が非常に遅いため、実機通信確認にはCubeIDEのRelease構成（`-Os`）を使用する。

## ビルドと書き込み

STM32CubeIDEで `Release` 構成を選び、Clean Project後にBuildする。生成されるファイルは `Release/stm32h753_Nucleo.elf` である。ST-LINKデバッガから同ELFを書き込み、mainループへ到達することを確認する。

書き込み後、リンクとIP疎通を確認する。

```sh
ping -c 4 192.168.2.10
```

## TLS Echoテスト

PCから次を実行する。

```sh
python3 doc/tcp_echo/test_tcp_echo.py \
  --host 192.168.2.10 \
  --port 4433 \
  --token stm32h753 \
  --size 4096 \
  --repeat 3 \
  --timeout 20
```

テストは不正トークン拒否、分割AUTH、AUTHとデータの同時送信、UTF-8テキスト、バイナリデータの完全一致を確認する。テストクライアントは開発用自己署名証明書を使用するため証明書検証を無効化している。本番試験では信頼するCA／証明書を設定し、検証を有効にすること。

## 実機確認結果

- Ping: 6送信、6受信、パケット損失0%
- TLS: TLS 1.2ハンドシェイク成功
- 不正トークン: 拒否成功
- 分割AUTH／AUTHと同時データ: 成功
- UTF-8テキストEcho: 42/42バイト一致
- バイナリEcho: 4096/4096バイト一致、3接続連続成功
