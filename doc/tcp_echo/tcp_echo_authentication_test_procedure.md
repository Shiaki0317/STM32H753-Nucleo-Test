# TCP Echo認証機能 確認手順書

## 1. 確認目的

次の3点を確認する。

1. 正しいtokenでは認証後にEchoできる。
2. 誤ったtokenではEchoせず切断する。
3. 認証追加後も大きなdataと再接続で通信が安定する。

## 2. 前提条件

- branch：`feature/tcp_echo`
- board：NUCLEO-H753ZI
- board IPv4 address：`192.168.2.10`
- TCP port：`7`
- PCとboardが同一networkに接続されている
- STM32CubeIDEからDebug buildと書き込みができる
- Python 3が利用できる

## 3. tokenの設定

既定の動作確認用tokenは`stm32h753`である。変更する場合はSTM32CubeIDEで次のpreprocessor symbolを設定する。

```text
TCP_ECHO_AUTH_TOKEN=\"任意のtoken\"
```

Project PropertiesのC/C++ Build設定でDebug configurationに追加し、Clean Buildする。tokenには改行を含めない。

注意：command history、build log、repositoryへ実際の製品credentialを残さないこと。製品ではcompile-time固定値以外の安全な保管方式を検討する。

## 4. buildと書き込み

1. STM32CubeIDEでprojectを開く。
2. Build Configurationを`Debug`にする。
3. `Project > Clean`を実行する。
4. `Project > Build Project`を実行する。
5. errorが0件であることを確認する。
6. Debugを開始し、`main()`到達後にResumeする。

## 5. network疎通確認

```bash
ping -c 4 192.168.2.10
```

packet lossが0%であることを確認する。失敗する場合は認証試験へ進まず、link、PC側IP address、board側IP addressを確認する。

## 6. 自動test

既定tokenの場合：

```bash
python3 doc/tcp_echo/test_tcp_echo.py
```

tokenを変更した場合：

```bash
python3 doc/tcp_echo/test_tcp_echo.py --token '設定したtoken'
```

testは最初に誤tokenの拒否を確認し、その後に正しいtokenでUTF-8 textとbinary dataを確認する。期待結果は次の形式である。

```text
invalid-token: rejected=yes
auth-stream: split=yes combined-data=yes
text: ... match=yes
binary[1/3]: ... match=yes
binary[2/3]: ... match=yes
binary[3/3]: ... match=yes
PASS: all TCP echo tests succeeded
```

## 7. 負荷確認

```bash
python3 doc/tcp_echo/test_tcp_echo.py \
  --token '設定したtoken' \
  --size 16384 \
  --repeat 10 \
  --timeout 5
```

全接続で`match=yes`となることを確認した後、再度pingを実行する。認証処理を追加してもEthernet RX bufferが回収され、serverが応答を継続することを確認するためである。

## 8. 手動確認

`nc`を使用する場合は、接続後に認証行とdataを入力する。

```bash
printf 'AUTH stm32h753\r\nhello\r\n' | nc 192.168.2.10 7
```

期待出力：

```text
OK
hello
```

誤token確認：

```bash
printf 'AUTH wrong-token\r\nhello\r\n' | nc 192.168.2.10 7
```

期待出力は`ERR authentication failed`のみで、`hello`は返らない。

## 9. debugger確認項目

必要に応じて次のfunctionへbreakpointを置く。

| function | 確認内容 |
|---|---|
| `TCP_Echo_Accept()` | 接続stateが確保される |
| `TCP_Echo_Authenticate()` | 認証行が分割されても解析される |
| `TCP_Echo_Flush()` | `OK`/`ERR`がEcho dataより先にqueueされる |
| `TCP_Echo_Poll()` | 未認証接続がtimeoutする |
| `TCP_Echo_Error()` | reset時にstateとpbufを解放する |

正常系では`auth_state`が`TCP_ECHO_AUTHENTICATED`になり、不正tokenでは`TCP_ECHO_AUTH_REJECTED`と`close_pending = 1`になることを確認する。

## 10. 合格条件

- firmwareのclean buildが成功する。
- pingが成功する。
- 誤tokenが拒否され、入力dataがEchoされない。
- 正しいtokenに`OK`が返る。
- 認証後のtext/binary dataがbyte単位で一致する。
- 16 KiB × 10接続後もpingと新規接続に応答する。

## 11. 今回の実機確認結果

2026年8月15日にNUCLEO-H753ZIで次を確認した。

| 確認項目 | 結果 |
|---|---|
| STM32CubeIDE Debug Clean Build | 0 errors、既存warning 13件 |
| 書き込み後ping | 6/6応答、packet loss 0% |
| 誤token | `ERR authentication failed`、Echoなし |
| TCP stream境界 | 分割認証と認証直後dataを確認 |
| UTF-8 text | 42/42 byte一致 |
| binary標準試験 | 4,096 byte × 3接続、全一致 |
| binary負荷試験 | 16,384 byte × 10接続、全一致 |
| 負荷試験後ping | 6/6応答、packet loss 0% |

以上から、簡易認証の追加後もTCP EchoとEthernet受信処理が継続動作することを確認した。
