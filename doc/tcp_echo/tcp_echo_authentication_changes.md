# TCP Echo認証機能 変更説明書

## 1. 目的と全体像

TCP Echo serverへ接続したclientが、Echo dataを送信する前に事前共有tokenで認証を受けるように変更した。

```text
Client                         STM32 TCP Echo server
  |---- TCP connect -------------------->|
  |---- AUTH <token>\r\n ---------------->|
  |<--- OK\r\n ----------------------------|  認証成功
  |---- application data ------------->|
  |<--- same application data ----------|
```

tokenが一致しない場合は`ERR authentication failed\r\n`を返して接続を閉じる。認証行を約10秒以内に完了しない場合は`ERR authentication timeout\r\n`を返して接続を閉じる。

## 2. 認証方式を選んだ理由

本projectはOSなしのlwIP Raw API構成であり、callback内で待機できない。このため、接続ごとに認証状態を保持する小さなstate machineを追加した。暗号libraryや追加threadを必要とせず、既存の非同期TCP処理を維持できる。

ただし、この方式は平文TCP上の簡易認証である。tokenはnetwork上で暗号化されず、firmware binaryにも含まれるため、開発環境または信頼できる閉域LANで接続制限を確認する目的に限定する。internetや第三者が接続可能なnetworkではTLS、device固有credential、安全なkey保管、token更新機構を使用すること。

## 3. protocol仕様

| 項目 | 内容 |
|---|---|
| 認証要求 | `AUTH <token>\r\n` |
| 成功応答 | `OK\r\n` |
| token不一致・形式不正 | `ERR authentication failed\r\n`送信後に切断 |
| timeout | `ERR authentication timeout\r\n`送信後に切断 |
| 最大認証行長 | 80 byte |
| 再試行 | 同じ接続内では不可。再接続が必要 |
| 認証後 | 従来どおり受信dataを同一内容で返す |

認証行と最初のEcho dataを同じTCP segmentで受信しても、改行より後のdataは失われずEcho対象になる。TCPはmessage境界を持たないため、認証行が複数segmentへ分割された場合にも対応する。

## 4. source codeの変更内容

### 4.1 `LWIP/App/tcp_echo.h`

`TCP_ECHO_AUTH_TOKEN`を追加した。

```c
#ifndef TCP_ECHO_AUTH_TOKEN
#define TCP_ECHO_AUTH_TOKEN "stm32h753"
#endif
```

既定値は動作確認用である。compilerのpreprocessor symbolで上書きできるため、sourceを直接変更せず環境ごとのtokenを指定できる。ただしmacroへ設定した値も最終的にはfirmwareへ格納される。

### 4.2 `LWIP/App/tcp_echo.c`

接続状態へ次を追加した。

- 認証待ち、認証済み、拒否済みの状態
- 分割受信された認証行の一時buffer
- 認証timeout用poll counter
- `OK`または`ERR`応答の送信位置

`TCP_Echo_Authenticate()`はpbufから1 byteずつ認証行を取り出す。認証に消費したbyteだけ`tcp_recved()`で受信窓を戻し、改行後に残ったpbufはEcho queueへ渡す。

`TCP_Echo_Flush()`はcontrol応答を先に送信し、認証済みの場合だけEcho dataを送信する。`tcp_sndbuf()`不足や`ERR_MEM`では既存と同様にsent/poll callbackから再試行するため、callback内でblockしない。

`TCP_Echo_Poll()`は未認証状態の接続時間を数える。timeoutを設けることで、接続だけを維持するclientによるconnection stateの占有を制限する。

### 4.3 `doc/tcp_echo/test_tcp_echo.py`

各正常系接続でEcho開始前に認証し、`OK\r\n`を確認するよう変更した。また、誤ったtokenが拒否されるnegative testを追加した。`--token`で試験用tokenを指定できる。

## 5. security上の意味と制限

この変更で「tokenを知らないclientへEcho serviceを提供しない」制御は追加されるが、次の攻撃を防ぐ完全なsecurity機能ではない。

- packet captureによるtoken盗聴
- tokenの再送によるなりすまし
- firmware解析による既定token取得
- 大量接続によるDoS
- 接続元deviceの真正性確認

製品化では、少なくとも既定tokenを使用せず、network firewallまたはVLANで接続元を制限する。機密性と相互認証が必要なら、TLSと安全なcredential provisioningを設計する。

## 6. 変更対象外

- CubeMX `.ioc`設定
- Ethernet MAC、RMII、LAN8742A設定
- lwIP memory parameter
- IPv4 addressとTCP port 7
- DMAおよびD-Cache整合処理
