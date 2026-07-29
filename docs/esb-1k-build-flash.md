# Totem + Prospector ESB 빌드 및 플래시

이 문서는 동일한 Seeed XIAO BLE/nRF52840 하드웨어에서 기존 ZMK BLE split 빌드와 ESB split 빌드를 구분하여 빌드하고 플래시하는 방법을 설명한다.

> 현재 ESB 구현은 USB HID polling interval을 1 ms로 설정하고 fresh-event 계측 경로를 포함한다. 현재 작업 브랜치의 10개 Actions matrix 결과는 아직 대기/미검증이며, 실제 키보드에서 USB cadence, radio latency/loss, 양쪽 동시 부하, 화면 및 battery를 모두 **미측정** 상태로 둔다. 빌드 성공 또는 `bInterval=1`만으로 “1K fresh radio event” 달성을 의미하지 않는다.

## 1. 빌드 프로필과 파일

`build.yaml`은 기존 BLE/초기화 빌드를 보존하면서 ESB release와 benchmark를 추가한 총 10개 항목을 정의한다.

| 용도 | board | shield | Actions UF2 파일 |
| --- | --- | --- | --- |
| 기존 BLE 왼쪽 | `xiao_ble//zmk` | `totem_left` | `totem_left-xiao_ble__zmk-zmk.uf2` |
| 기존 BLE 오른쪽 | `xiao_ble//zmk` | `totem_right` | `totem_right-xiao_ble__zmk-zmk.uf2` |
| 기존 BLE Prospector 동글 | `xiao_ble//zmk` | `totem_dongle prospector_adapter` | `totem_dongle prospector_adapter-xiao_ble__zmk-zmk.uf2` |
| 설정 초기화 | `xiao_ble//zmk` | `settings_reset` | `settings_reset-xiao_ble__zmk-zmk.uf2` |
| ESB 왼쪽, wire ID 1 | `xiao_ble//zmk` | `totem_left totem_esb_left` | `totem_left_esb.uf2` |
| ESB 오른쪽, wire ID 2 | `xiao_ble//zmk` | `totem_right totem_esb_right` | `totem_right_esb.uf2` |
| ESB Prospector USB 동글 | `xiao_ble//zmk` | `totem_dongle prospector_adapter totem_esb_dongle` | `totem_dongle_esb_prospector.uf2` |
| ESB benchmark 왼쪽 | `xiao_ble//zmk` | `totem_left totem_esb_left totem_esb_benchmark` | `totem_left_esb_benchmark.uf2` |
| ESB benchmark 오른쪽 | `xiao_ble//zmk` | `totem_right totem_esb_right totem_esb_benchmark` | `totem_right_esb_benchmark.uf2` |
| ESB benchmark Prospector 동글 | `xiao_ble//zmk` | `totem_dongle prospector_adapter totem_esb_dongle totem_esb_benchmark` | `totem_dongle_esb_prospector_benchmark.uf2` |

로컬 빌드의 원본 출력은 각 빌드 디렉터리 아래 `zephyr/zmk.uf2`이다. 아래 명령은 역할별 파일명으로 복사하지 않으므로, 디렉터리 이름으로 역할을 구분해야 한다.

ESB release는 press debounce 1 ms, release debounce 5 ms를 사용한다. Benchmark는 transport 부하를 분리하기 위해 press/release를 모두 0 ms로 바꾸고 per-packet RTT logging과 1,000 µs synthetic producer를 켠다. Release와 현재 benchmark 동글은 모두 Prospector 화면을 유지한다. Display-disabled artifact는 없다.

## 2. GitHub Actions 빌드

`.github/workflows/build.yml`은 다음 경우 실행된다.

- 브랜치에 push
- pull request 생성 또는 갱신
- GitHub의 **Actions → Build → Run workflow**에서 수동 실행

수동 실행 시 ESB 작업 브랜치 `feature/totem-prospector-esb-1k`를 선택한다. 전체 matrix에서 위의 10개 빌드가 모두 성공했는지 확인한다. 특히 ESB release/benchmark뿐 아니라 기존 BLE 세 개와 `settings_reset`도 확인해야 롤백 가능성을 검증할 수 있다. 이 문서 작성 시점에는 새 10개 matrix의 성공 결과가 없다.

완료된 run의 **Artifacts**에서 `firmware`를 다운로드하고 ZIP을 푼다. ZIP 파일 자체는 XIAO에 복사하지 않는다. 압축을 푼 뒤 역할에 맞는 `.uf2` 파일을 사용한다.

새 ESB 프로필의 CI가 성공하기 전에는 예정 파일명을 보고 빌드 성공으로 간주하지 않는다. 빌드의 `zephyr/.config` 또는 Kconfig 출력에서 동글에 다음 값이 들어갔는지도 확인한다.

```text
CONFIG_ZMK_USB=y
CONFIG_USB_HID_POLL_INTERVAL_MS=1
CONFIG_ZMK_SPLIT_ESB=y
CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT=2
```

왼쪽과 오른쪽에서는 ID와 역할별 hardware retry delay를 확인한다.

```text
CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID=1
CONFIG_ZMK_SPLIT_ESB_PROTO_TX_RETRANSMIT_DELAY=500
CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID=2
CONFIG_ZMK_SPLIT_ESB_PROTO_TX_RETRANSMIT_DELAY=800
CONFIG_ZMK_SPLIT_ESB_PROTO_TX_RETRANSMIT_COUNT=3
CONFIG_ZMK_SPLIT_ESB_RF_CH_HOP=n
```

공통 설정은 pipe 3개, hardware ACK, command application retry 0이어야 한다.

```text
CONFIG_ESB_PIPE_COUNT=3
CONFIG_ZMK_SPLIT_ESB_PROTO_TX_ACK=y
CONFIG_ZMK_SPLIT_ESB_RETRY_CMD=0
```

## 3. 고정 dependency

| 구성요소 | revision |
|---|---|
| ZMK | `904c9aec8822d79149d42c8a9a77e8828eb08f5a` |
| Zephyr | `9df4b12b5af3438a8b9d7a33780dc3b3b2f516c1` |
| zmk-feature-split-esb | `1f4cd4558bb9e0626ec2507f334f239862af859d` |
| sdk-nrf | `9b3d2623fdcd9c0fd0284f860beea924568c9826` |
| nrfxlib | `dfadf17305d8f000eda9aa74a5b9ff1c5647a23e` |
| Prospector | `ed98221f3b52b7066dbb10ba3af8a29150b93a5a` |
| zmk-tri-state | `2007896c6d5bfb519e8babccf8633841c5647d8b` |

Zephyr는 ZMK가 import하는 branch 대신 main manifest의 exact SHA override를 사용한다. Reusable Actions workflow도 위 ZMK SHA로 고정되어 있다.

## 4. 로컬 빌드 준비

ZMK가 요구하는 Zephyr SDK/toolchain, Python, Git 및 `west`를 설치한 PowerShell 환경을 사용한다. Dependency checkout과 build output이 작업 중인 원본 저장소에 섞이지 않도록, 원본 저장소 밖에 별도 west workspace를 만들고 그 안의 `config` 경로에 작업 브랜치를 clone한다.

```powershell
$workspace = Join-Path $env:USERPROFILE 'zmk-esb-workspace'
New-Item -ItemType Directory -Force -Path $workspace | Out-Null

git clone --branch feature/totem-prospector-esb-1k `
  https://github.com/asj9005/zmk-config-pro.git "$workspace\config"

Set-Location $workspace
west init -l --mf config/west.yml config
west update --fetch-opt=--filter=tree:0
west zephyr-export

$repo = (Resolve-Path .\config).Path
```

이미 workspace를 만든 경우 새로 clone/init하지 말고 해당 `config` clone을 원하는 commit으로 갱신한 뒤 `west update`를 실행한다. 위 명령은 commit/push된 branch를 대상으로 하므로 원본 working tree의 uncommitted 변경은 포함하지 않는다.

이 저장소 자체도 Zephyr extra module이며 configure 중 pinned upstream ESB/Prospector checkout에 compatibility overlay를 적용한다. 같은 west workspace에서 여러 build를 병렬 실행하면 같은 dependency 파일을 동시에 바꿀 수 있으므로 아래 build는 반드시 **순차 실행**한다. 모든 로컬 build에 다음 두 CMake 인수를 전달한다.

```text
-DZMK_CONFIG=<저장소>\config
-DZMK_EXTRA_MODULES=<저장소>
```

경로에 공백이나 한글이 있을 수 있으므로 아래 PowerShell 예시처럼 인수 전체를 따옴표로 묶는다.

## 5. ESB release firmware 로컬 빌드

### 왼쪽

```powershell
west build -p always -s zmk/app -d build/totem_left_esb -b "xiao_ble//zmk" -- `
  "-DSHIELD=totem_left totem_esb_left" `
  "-DZMK_CONFIG=$repo\config" `
  "-DZMK_EXTRA_MODULES=$repo"
```

결과: `build/totem_left_esb/zephyr/zmk.uf2`

### 오른쪽

```powershell
west build -p always -s zmk/app -d build/totem_right_esb -b "xiao_ble//zmk" -- `
  "-DSHIELD=totem_right totem_esb_right" `
  "-DZMK_CONFIG=$repo\config" `
  "-DZMK_EXTRA_MODULES=$repo"
```

결과: `build/totem_right_esb/zephyr/zmk.uf2`

### Prospector ESB USB 동글

```powershell
west build -p always -s zmk/app -d build/totem_dongle_esb_prospector `
  -b "xiao_ble//zmk" -S studio-rpc-usb-uart -- `
  "-DSHIELD=totem_dongle prospector_adapter totem_esb_dongle" `
  "-DCONFIG_ZMK_STUDIO=y" `
  "-DZMK_CONFIG=$repo\config" `
  "-DZMK_EXTRA_MODULES=$repo"
```

결과: `build/totem_dongle_esb_prospector/zephyr/zmk.uf2`

release 동글은 Prospector 화면을 활성화하고 USB HID polling interval을 1 ms로 명시한다. 이 설정은 USB host의 1 ms polling 기회를 구성하지만, 좌우 하프에서 새 이벤트가 실제로 1 ms마다 도착함을 증명하지 않는다.

## 6. 기존 BLE 및 settings reset 로컬 빌드

ESB에서 BLE로 롤백할 수 있도록 다음 빌드도 유지한다.

```powershell
west build -p always -s zmk/app -d build/totem_left_ble -b "xiao_ble//zmk" -- `
  "-DSHIELD=totem_left" `
  "-DZMK_CONFIG=$repo\config" `
  "-DZMK_EXTRA_MODULES=$repo"

west build -p always -s zmk/app -d build/totem_right_ble -b "xiao_ble//zmk" -- `
  "-DSHIELD=totem_right" `
  "-DZMK_CONFIG=$repo\config" `
  "-DZMK_EXTRA_MODULES=$repo"

west build -p always -s zmk/app -d build/totem_dongle_ble `
  -b "xiao_ble//zmk" -S studio-rpc-usb-uart -- `
  "-DSHIELD=totem_dongle prospector_adapter" `
  "-DCONFIG_ZMK_STUDIO=y" `
  "-DZMK_CONFIG=$repo\config" `
  "-DZMK_EXTRA_MODULES=$repo"

west build -p always -s zmk/app -d build/settings_reset -b "xiao_ble//zmk" -- `
  "-DSHIELD=settings_reset" `
  "-DZMK_CONFIG=$repo\config" `
  "-DZMK_EXTRA_MODULES=$repo"
```

각 결과는 해당 `build/<이름>/zephyr/zmk.uf2`에 생성된다.

## 7. Benchmark firmware

`totem_esb_benchmark` auxiliary shield를 역할 shield 뒤에 추가하면 RTT benchmark logging과 synthetic event를 활성화한다. 기본 synthetic period는 1,000 µs이다. 이 프로필은 transport 자체를 분리 측정하기 위해 debounce를 0 ms로 바꾸므로 일상 사용용 firmware가 아니다. Benchmark도 Prospector 화면을 켜며 display-disabled 변형은 아직 없다.

Actions artifact의 benchmark UF2 세 개는 앞 표에 있다. 로컬에서는 다음을 순차 실행한다.

```powershell
west build -p always -s zmk/app -d build/totem_left_esb_benchmark `
  -b "xiao_ble//zmk" -- `
  "-DSHIELD=totem_left totem_esb_left totem_esb_benchmark" `
  "-DZMK_CONFIG=$repo\config" `
  "-DZMK_EXTRA_MODULES=$repo"

west build -p always -s zmk/app -d build/totem_right_esb_benchmark `
  -b "xiao_ble//zmk" -- `
  "-DSHIELD=totem_right totem_esb_right totem_esb_benchmark" `
  "-DZMK_CONFIG=$repo\config" `
  "-DZMK_EXTRA_MODULES=$repo"

west build -p always -s zmk/app -d build/totem_dongle_esb_prospector_benchmark `
  -b "xiao_ble//zmk" -S studio-rpc-usb-uart -- `
  "-DSHIELD=totem_dongle prospector_adapter totem_esb_dongle totem_esb_benchmark" `
  "-DCONFIG_ZMK_STUDIO=y" `
  "-DZMK_CONFIG=$repo\config" `
  "-DZMK_EXTRA_MODULES=$repo"
```

Benchmark는 deferred log buffer 8 KiB, RTT up buffer 4 KiB와 DROP mode를 사용한다. Logging과 추가 RAM이 release timing을 바꾸므로 benchmark 수치를 release firmware 수치로 바꾸어 쓰지 않는다. Logger drop이 있는 capture로 loss 0을 주장하지 않는다. 실제 계측 방법과 결과 기록 형식은 `docs/esb-1k-benchmark.md`를 따른다.

## 8. ESB transport 동작 요약

- Pipe 0은 예약, pipe 1은 왼쪽, pipe 2는 오른쪽이다.
- Pipe 1/2는 각각 half uplink와 그 uplink ACK에 실리는 reverse command payload를 함께 사용한다.
- Dongle PRX는 command를 선제 송신할 수 없다. 대상 half의 다음 event/heartbeat가 command 전달 기회다.
- Command application retry는 0이다. Key/sensor/battery event application retry는 3이다.
- 대상 half가 ACK command를 소비하기 전에 꺼지면 pipe별 selective flush가 없는 hardware FIFO entry가 다른 half의 reverse command를 지연시킬 수 있다. Key uplink 자체와는 별도인 알려진 한계다.
- Hardware retransmit count는 3이며 delay는 left 500 µs, right 800 µs다.
- RF hopping은 비활성화되어 있다.
- 서로 다른 pipe는 병렬 radio가 아니다. TDMA/CSMA가 없으므로 양쪽 동시 1 kHz fresh delivery는 실측 전까지 미보장이다.
- 각 half는 boot 때 random non-zero session ID를 만들며, dongle은 session 변경 또는 peer timeout 때 그 source의 held key를 release하고 sequence state를 재설정한다.
- Peer transition이나 battery event 뒤 Prospector 상태는 100 ms 후 source 0/1을 500 ms 간격으로 한 번씩 재게시한다. 안정 상태의 무한 refresh loop는 사용하지 않는다.

## 9. ESB로 처음 전환하는 플래시 순서

모든 장치에 충분한 전원을 공급하고 역할별 파일을 혼동하지 않는다. XIAO BLE의 reset 버튼을 빠르게 두 번 눌러 UF2 bootloader 드라이브가 나타나면 해당 `.uf2`를 드라이브에 복사한다. 복사 후 드라이브가 자동으로 사라지고 장치가 재부팅될 수 있다.

기존 BLE bond와 저장 설정이 역할 전환에 영향을 주지 않도록 최초 전환 시 다음 순서를 권장한다.

1. 동글, 왼쪽, 오른쪽 각각에 `settings_reset-xiao_ble__zmk-zmk.uf2`를 한 번씩 플래시한다.
2. 왼쪽에 `totem_left_esb.uf2`를 플래시한다.
3. 오른쪽에 `totem_right_esb.uf2`를 플래시한다.
4. PC에 연결할 Prospector 동글에 `totem_dongle_esb_prospector.uf2`를 마지막으로 플래시한다.
5. 동글을 USB로 다시 연결하고 keyboard HID enumeration과 Prospector 화면 초기화를 확인한다.
6. 왼쪽과 오른쪽을 각각 입력하여 양쪽 모두 동작하는지 확인한다.

`settings_reset`은 저장 설정을 지우는 전용 firmware이다. 정상 키보드 firmware가 아니므로 초기화가 끝난 뒤 반드시 해당 장치의 역할 firmware를 다시 플래시한다. 매번 firmware를 갱신할 때 초기화할 필요는 없지만, BLE↔ESB transport 변경이나 역할 변경 후 이상 상태가 남으면 다시 수행한다.

왼쪽/오른쪽 ESB firmware는 USB host keyboard 출력용이 아니다. PC에는 Prospector ESB 동글만 연결하여 사용한다.

## 10. 기존 BLE 빌드로 롤백

원래 BLE split 구조로 돌아가려면 세 장치를 한 세트로 롤백한다. ESB peripheral과 BLE central 또는 그 반대 조합은 서로 통신하지 않는다.

1. 동글, 왼쪽, 오른쪽 각각에 `settings_reset-xiao_ble__zmk-zmk.uf2`를 플래시한다.
2. 왼쪽에 `totem_left-xiao_ble__zmk-zmk.uf2`를 플래시한다.
3. 오른쪽에 `totem_right-xiao_ble__zmk-zmk.uf2`를 플래시한다.
4. Prospector 동글에 `totem_dongle prospector_adapter-xiao_ble__zmk-zmk.uf2`를 플래시한다.
5. 세 장치를 재부팅하고 기존 ZMK BLE split 연결과 Prospector 상태를 확인한다.

기존 `totem-prospector` 브랜치는 변경하지 않으므로, 필요하면 그 브랜치의 성공한 Actions run에서 기존 firmware를 다시 받을 수도 있다.

## 11. ESB 주소 변경

세 역할이 공유하는 주소는 한 파일에만 정의되어 있다.

```text
config/boards/shields/totem/totem_esb_addr.dtsi
```

현재 이 Totem 세트용 값은 다음과 같다.

```text
base-addr-0 = 3B F1 26 68
base-addr-1 = FB A3 A7 62
addr-prefix = 69 D4 8A
```

`addr-prefix`는 세 값을 유지한다. 첫 값은 예약 pipe 0, 둘째는 left pipe 1, 셋째는 right pipe 2에 해당한다. 다른 ESB 키보드와 충돌을 피하려고 값을 바꿀 때는 이 파일만 수정한 뒤 왼쪽, 오른쪽, 동글 세 firmware를 모두 다시 빌드하고 함께 플래시한다. 서로 다른 주소로 빌드된 장치는 통신하지 않는다. 예제 프로젝트의 기본 주소를 그대로 재사용하지 않는다.

ESB 주소는 비밀키가 아니다. 현재 transport는 payload 암호화, 송신자 인증 및 replay 방지를 제공하지 않는다. CRC와 ACK는 전송 오류와 수신 여부를 확인할 뿐 보안 기능이 아니다. 주소를 아는 근거리 장치가 packet을 관찰하거나 위조할 수 있다는 제한을 전제로 사용한다.

## 12. 전원 및 아직 확인되지 않은 항목

ESB 2.4 GHz의 빠른 전송, +8 dBm 출력, ACK/retry 및 짧은 scan/debounce 설정은 기존 BLE split보다 배터리 사용량을 늘릴 수 있다. 좌우 하프의 sleep과 battery reporting은 가능한 범위에서 유지되지만, 실제 사용 시간은 아직 측정하지 않았다.

Prospector release 화면, 왼쪽/오른쪽 연결 상태 및 battery event 전달 코드 경로는 포함되어 있다. 그러나 실제 장치에서 다음 항목은 아직 검증하지 않았다.

- 현재 branch의 10개 Actions matrix build와 artifact
- USB keyboard enumerate 및 Full-Speed 동작
- Prospector 화면의 정상 초기화와 지속 동작
- 왼쪽/오른쪽 배터리 값의 정확한 표시
- USB descriptor와 USBPcap에서의 실제 1 ms interrupt cadence
- 왼쪽/오른쪽 fresh ESB event의 typical, p95, p99 latency
- 양쪽 동시 부하 및 100,000 synthetic event에서의 packet loss
- RF 혼잡 시 retry, sequence gap 및 queue overflow
- half reboot/session 변경과 timeout 때 sequence reset 및 stuck-key release
- 화면 on/off가 radio 및 USB latency에 미치는 영향; display-disabled build도 미구현

실측 전에는 위 값을 추정치나 성공 수치로 기록하지 않는다.
