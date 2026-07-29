# Totem + Prospector ESB 1K 벤치마크

## 1. 이 문서에서 말하는 1K

서로 다른 네 가지를 분리해서 기록한다.

1. **USB endpoint polling 설정**: USB Full-Speed HID interrupt-IN endpoint descriptor의 `bInterval=1`, 즉 host가 1 ms 간격으로 service할 수 있는 설정이다.
2. **ESB fresh-event delivery**: left와 right가 각각 새 sequence를 가진 event를 만들고 dongle이 그 sequence를 실제로 수신한 간격이다.
3. **Dongle 내부 RX→USB queue 구간**: 같은 dongle clock으로 기록한 key event RX와 keyboard-report 함수 enter/return 사이의 시간이다. Host 수신 시각은 포함하지 않는다.
4. **end-to-end latency**: switch 검출부터 host가 HID report를 받기까지의 시간이다. debounce, queue, radio retry, ZMK behavior 처리와 USB 대기가 모두 포함된다.

`bInterval=1`만 확인하거나 같은 HID report를 1 ms마다 반복한 결과는 fresh radio 1K의 증거가 아니다. 이 저장소의 synthetic benchmark packet(`wire=2`)은 radio transport를 검증하지만 HID report를 생성하지도 않는다. 따라서 radio와 USB 결과를 각각 측정하고, end-to-end가 필요하면 별도의 상관 가능한 hardware 계측을 사용한다.

이 문서에는 실측값을 미리 채우지 않는다. 구현 commit `3cf5bb610187ab2e84330703c28fb9a79c5c97ec`의 [Actions run 30433480301](https://github.com/asj9005/zmk-config-pro/actions/runs/30433480301)은 10/10 빌드에 성공했다. 장치에서 얻지 않은 값은 결과 표에 **미측정**으로 남긴다.

## 2. firmware 계측 경로

`totem_esb_benchmark` auxiliary shield를 ESB 역할 shield 뒤에 추가하면 다음 설정이 켜진다.

- `CONFIG_TOTEM_ESB_BENCHMARK=y`
- RTT deferred logging
- log buffer 4 KiB, RTT up buffer 4 KiB, DROP mode
- 기본 synthetic period 1,000 µs
- benchmark 전용 0 ms debounce

역할별 shield 조합은 다음과 같다.

| 역할 | benchmark shield 조합 |
|---|---|
| left | `totem_left totem_esb_left totem_esb_benchmark` |
| right | `totem_right totem_esb_right totem_esb_benchmark` |
| Prospector dongle | `totem_dongle prospector_adapter totem_esb_dongle totem_esb_benchmark` |

이 세 조합은 `build.yaml`의 Actions matrix와 로컬 build에서 모두 만들 수 있다. 검증된 artifact는 `totem_left_esb_benchmark.uf2`, `totem_right_esb_benchmark.uf2`, `totem_dongle_esb_prospector_benchmark.uf2`다. Dongle benchmark는 Prospector 화면과 USB HID를 유지하지만 Studio RPC/CDC snippet은 사용하지 않고 계측 출력은 RTT로만 보낸다. Release dongle은 기존처럼 Studio를 유지한다.

0 ms debounce는 transport만 분리해서 보는 benchmark 전용 값이다. Release ESB firmware의 기본 press 1 ms/release 5 ms 결과로 간주하면 안 된다. 현재 benchmark 동글도 Prospector 화면을 유지하며 display-disabled artifact는 구현되지 않았다.

기본 1,000 µs synthetic period에서 한 half당 100,000개의 event를 만들려면 warm-up을 제외하고 최소 약 100초가 필요하다. Delayable work scheduling, collision, retry 및 queue 부하 때문에 실제 시간은 더 길 수 있다. 실제 시작·종료는 동일 session의 sequence와 accepted fresh count로 판단하고, 시간만으로 성공 건수를 추정하지 않는다.

### 로그 record

Dongle은 수신 packet마다 다음 형식을 출력한다.

```text
BENCH_RX source=0 session=305419896 seq=123 gap=0 source_tick=123456 dongle_tick=789012 wire=2 event=0 position=0 pressed=0 accepted=0
```

| field | 의미 |
|---|---|
| `source` | ZMK logical source. `0=left`, `1=right` |
| `session` | 해당 half가 boot 때 hardware entropy로 만든 non-zero uint32 session ID |
| `seq` | 해당 session에서 모든 wire event가 공유하는 uint32 monotonic sequence |
| `gap` | 같은 source/session에서 dongle이 앞서 수락한 sequence와 비교한 누락 수 |
| `source_tick` | peripheral에서 packet을 만든 `k_cycle_get_32()` 값 |
| `dongle_tick` | dongle에서 packet을 처리한 `k_cycle_get_32()` 값 |
| `wire` | `0=ZMK event`, `1=heartbeat`, `2=synthetic benchmark` |
| `event` | `wire=0`일 때 `0=key`, `1=sensor`, `2=input`, `3=battery` |
| `position`, `pressed` | key-position event의 위치와 press/release |
| `accepted` | Fresh `wire=0` event가 active ESB transport를 통해 ZMK handler로 전달되면 1 |

`accepted=0`이 곧 radio packet 거부를 뜻하지는 않는다. Heartbeat와 synthetic `wire=2`는 정상 수신돼도 ZMK event handler로 보내지 않으므로 0이다. 분석기는 synthetic fresh count를 이 field가 아니라 source/session별 sequence 규칙으로 계산한다. Session이 바뀌면 dongle은 sequence 기준을 재설정하고 해당 source에서 눌린 것으로 남은 key를 release한다. Peer timeout 때도 held key release와 sequence reset을 수행한다.

Invalid packet, RX overflow, heartbeat link counter 및 TX terminal 결과는 다음 형식이다.

```text
BENCH_RX_INVALID pipe=1 error=-74 dongle_tick=789012
BENCH_RX_OVERFLOW pipe=1 count=2 dongle_tick=789012
BENCH_LINK source=0 session=305419896 metric=1 value=100234 dongle_tick=789012
BENCH_TX source=0 msg=123 attempts=5 retransmissions=4 success=1 source_tick=123456
```

`BENCH_LINK` 값은 peripheral boot 이후 누적 counter이며 heartbeat가 metric 0~4를 하나씩 순환해 보낸다.

| metric | 이름 | 의미 |
|---:|---|---|
| 0 | `tx_messages` | terminal success/failure가 나온 TX message 수 |
| 1 | `tx_attempts` | application retry round 전체의 hardware transmission 수 |
| 2 | `tx_failures` | retry budget을 모두 소진한 terminal failure 수 |
| 3 | `app_queue_pressure` | ESB application message queue pressure 횟수 |
| 4 | `producer_queue_overflow` | producer TX ring에 packet을 넣지 못한 횟수 |

기본 heartbeat 250 ms에서는 같은 metric이 약 1.25초마다 갱신된다. 분석기는 source/metric별 마지막 session과 값을 보여 준다. Session이 바뀐 뒤에는 이전 누적값과 새 session 값을 더하지 않는다.

`BENCH_TX`는 dongle이 아니라 송신한 left/right peripheral의 RTT log에 기록된다. `attempts`는 한 logical message가 terminal success/failure에 도달할 때까지 모든 application retry round의 hardware transmission 수를 합한 값이며 `retransmissions=attempts-1`이다. Application retry round 수와 시점은 이 한 줄만으로 복원할 수 없다. `source_tick`은 TX 완료 callback 시점의 peripheral clock이고 packet 생성 시각이나 dongle 수신 시각이 아니다. `msg`는 16-bit local ID로 장시간 capture에서 재사용되므로 unique packet count로 쓰지 않는다.

Application parser 오류는 다음처럼 분리한다.

| 오류 | errno | 의미 |
|---|---|---|
| CRC32 mismatch | `-EBADMSG` (`-74`) | Application envelope checksum 불일치 |
| Magic prefix mismatch | `-EPROTO` (`-71`) | Envelope prefix 불일치 |
| Size/type mismatch | `-EMSGSIZE` (`-90`) | Payload 길이 또는 wire/event type 불일치 |

Invalid pipe/source는 별도 errno로 나타날 수 있다. `BENCH_RX_OVERFLOW`는 parser 오류와 별개인 RX ring 누적 overflow count다. Nordic radio CRC에서 hardware가 폐기한 frame 수를 이 로그가 직접 세지는 않는다.

Key-position RX와 dongle의 keyboard-report 호출을 FIFO로 연결한 record는 다음 형식이다.

```text
BENCH_USB source=0 session=305419896 seq=124 position=5 pressed=1 rx_tick=789012 queue_enter_tick=789100 queue_done_tick=789130 result=0
BENCH_USB_UNMATCHED queue_enter_tick=800000 queue_done_tick=800030 result=0
BENCH_USB_QUEUE_OVERFLOW source=0 seq=999
```

| field | 의미 |
|---|---|
| `session`, `seq` | 해당 key-position `BENCH_RX`와 같은 identity |
| `rx_tick` | 해당 key-position packet의 `BENCH_RX`를 기록한 dongle tick |
| `queue_enter_tick` | `zmk_usb_hid_send_keyboard_report()` wrapper 진입 직전 dongle tick |
| `queue_done_tick` | 실제 함수가 반환한 직후 dongle tick |
| `result` | 실제 keyboard-report 함수 반환값 |
| `UNMATCHED` | 대기 중인 remote key event 없이 keyboard report 함수가 호출됨 |
| `QUEUE_OVERFLOW` | Pending remote key FIFO에 넣지 못함 |

세 tick은 모두 dongle clock이므로 RX→queue enter, queue call, RX→queue done 차이를 계산할 수 있다. 그러나 이 계측은 remote key-position event와 keyboard-report 함수 호출을 FIFO 순서로 연결한다. 다음과 같은 경우에는 1:1 관계가 깨질 수 있다.

- hold-tap처럼 report가 즉시 만들어지지 않는 behavior
- layer key처럼 keyboard report가 없을 수 있는 key
- combo, sticky key와 report coalescing
- dongle 자체/local event나 다른 경로가 만든 report

따라서 `BENCH_USB` latency 분포는 일반 key 하나를 사용해 press 하나→report 하나, release 하나→report 하나가 명확한 단순 테스트에서만 유효하다. `UNMATCHED`, overflow 또는 event/report 수 불일치가 있으면 FIFO 표본을 end-to-end latency로 해석하지 않는다.

### clock에 관한 제한

Left, right, dongle의 tick counter는 서로 동기화되어 있지 않다. 다음 계산은 유효하다.

- 같은 peripheral의 연속 `source_tick` 차이: 생성 cadence
- 같은 dongle에서 source별 연속 `dongle_tick` 차이: 도착 cadence
- 같은 dongle의 `BENCH_USB` `rx_tick`→`queue_enter_tick`/`queue_done_tick`: 내부 queue 구간

다음 계산은 유효하지 않다.

- `dongle_tick - source_tick`: one-way latency
- left tick과 right tick의 절대값 비교

One-way 또는 end-to-end latency를 구하려면 clock 동기화·offset 보정이 있는 protocol, 또는 switch/GPIO와 USB를 함께 보는 logic analyzer가 필요하다.

## 3. ESB fresh-event 측정

### 준비

1. Benchmark left, right, dongle firmware를 flash한다.
2. Dongle, left, right를 reset하고 각 half가 새 random session을 시작했는지 확인한다. Session ID가 있으므로 half 단독 재부팅도 구분되지만, 한 capture 안의 결과는 source/session별로 분리한다.
3. Dongle RTT에는 `BENCH_RX`, `BENCH_LINK`, `BENCH_USB`, invalid/overflow가 기록된다. Left/right RTT에는 각각 source별 `BENCH_TX`가 기록된다. Retry 통계까지 필요하면 세 장치의 로그를 같은 run에서 수집하거나, 적어도 left/right 파일을 역할별로 따로 보관한다.
4. `CONFIG_LOG_BACKEND_RTT=y`이므로 USB HID capture 자체가 RTT 로그를 대신하지 않는다.
5. RTT consumer가 지속해서 로그를 비우는지 확인한다. Zephyr logger의 dropped-message 경고가 하나라도 있으면 그 capture로 `0 / 100,000 loss`나 완전한 retransmission 통계를 주장하지 않는다.
6. 처음 몇 초는 warm-up으로 버리거나 분석 대상 시작 session/sequence를 기록한다.

Benchmark 설정은 deferred log buffer 4 KiB, RTT up buffer 4 KiB 및 `CONFIG_LOG_BACKEND_RTT_MODE_DROP=y`를 사용한다. Dongle은 pending USB event 64개 FIFO도 추가한다. Benchmark dongle에서는 Studio RPC/CDC를 제외하여 4 KiB RPC stack과 불필요한 USB traffic을 없앤다. 매 packet 한 줄 logging은 radio/work queue timing과 RAM 사용량을 바꾸며, 2개 half가 동시에 1 kHz로 송신하면 logger가 packet 처리보다 먼저 포화될 수 있다. Logger drop, malformed marker, RX overflow, application queue pressure 또는 producer overflow가 하나라도 있으면 해당 capture를 무손실 검증으로 사용하지 않는다. 이 mode는 release timing 자체가 아니라 sequence/cadence와 실패 원인을 관찰하기 위한 계측 build다.

### 분석

저장소 루트의 PowerShell에서 다음을 실행한다.

```powershell
py -3 .\tools\analyze_esb_benchmark.py .\captures\esb-dongle.log
```

이 경우 source/session별 sequence와 cycle 단위 cadence, link metric, invalid/overflow 및 파일에 포함된 TX 결과를 계산한다. Left/right `BENCH_TX` 파일도 분석할 수 있으며 `source` field로 분리된다. 서로 다른 장치 로그의 행 순서나 timestamp를 합쳐 latency를 만들지는 않는다. 빌드의 실제 `k_cycle_get_32()` 주파수를 확인한 경우에만 Hz를 명시한다.

```powershell
py -3 .\tools\analyze_esb_benchmark.py .\captures\esb-dongle.log `
  --dongle-clock-hz <DONGLE_HZ> `
  --source-clock-hz <PERIPHERAL_HZ>
```

주파수를 모르면 옵션을 생략하고 cycles 결과를 기록한다. nRF52840의 CPU 주파수나 USB frame rate를 `k_cycle_get_32()` 주파수로 임의 대입하지 않는다.

분석기는 sequence integrity를 source/session별로 모든 wire type에 걸쳐 계산한다. Sequence는 ZMK, heartbeat, benchmark packet이 공유하기 때문에 `wire=2`만 골라 sequence gap을 계산하면 heartbeat가 거짓 gap으로 잡힌다. Session 변경은 sequence gap으로 세지 않는다. Fresh-event cadence는 session별로 중복·out-of-order를 제외한 `wire=2` packet만으로 계산하며 `BENCH_RX accepted` field를 synthetic acceptance로 오해하지 않는다.

`typical`은 median(p50)이다. `p95`, `p99`는 선형 보간 percentile이며, 사용한 interval 개수도 결과와 함께 보존한다.

### Dongle RX→USB queue 구간

이 구간은 synthetic stream과 별도로 실제 일반 key press/release로 측정한다.

1. Hold-tap, layer, combo, sticky 또는 mouse behavior가 아닌 일반 key 하나를 고른다.
2. 다른 key와 dongle local event가 없는 상태에서 일정한 간격으로 press/release한다.
3. `BENCH_RX wire=0 event=0 accepted=1`과 `BENCH_USB`의 source, session, sequence, position, pressed가 일치하는지 확인한다.
4. `BENCH_USB_UNMATCHED=0`, `BENCH_USB_QUEUE_OVERFLOW=0`, keyboard-report `result=0`인지 확인한다.
5. 분석기의 source별 `rx_to_queue_enter`, `queue_call_duration`, `rx_to_queue_done` typical/p95/p99를 기록한다.

이 값은 dongle이 radio key event를 받은 뒤 USB report 함수가 처리되는 내부 시간이다. USB host가 실제 payload를 받은 시각은 USBPcap 또는 hardware analyzer로 별도 측정한다. Synthetic `wire=2` packet은 ZMK key handler에 전달되지 않으므로 `BENCH_USB`를 생성하지 않는다.

### 조건별 실행

각 조건에서 source 0과 source 1을 별도로 기록한다.

1. Left만 켠 상태
2. Right만 켠 상태
3. Left와 right 동시 1 kHz synthetic 부하
4. 두 half 동시 부하 + 실제 key press/release
5. Prospector display enabled
6. Display-disabled 비교: 현재 artifact가 없어 실행하지 않음
7. 평상시 RF 환경
8. Wi-Fi/Bluetooth traffic을 의도적으로 늘린 RF 혼잡 환경

Pipe 1/2는 같은 channel과 단일 PRX radio를 공유한다. Protocol에는 TDMA/CSMA가 없다. Left 500 µs/right 800 µs 비대칭 hardware retry delay는 동시 충돌을 줄이기 위한 완화책일 뿐이므로, both 조건의 양쪽 1 kHz cadence나 loss 0을 사전에 기대값으로 확정하지 않는다.

현재 제공된 Prospector ESB release/benchmark 조합은 모두 화면을 유지한다. Display-disabled firmware는 **미구현**이며 on/off 비교는 **미측정**으로 남긴다. Release firmware에서 임의로 화면을 제거한 결과를 같은 artifact의 결과처럼 보고하지 않는다.

### 판정

Radio fresh-event 1K급을 주장하려면 적어도 다음이 모두 필요하다.

- source 0과 1 각각에서 분석기가 계산한 `wire=2 accepted_fresh`가 100,000개 이상
- 각 source/session 전체 sequence의 `missing_sequences=0`
- source별 fresh benchmark dongle-RX cadence의 typical/p95/p99 기록
- 양쪽 동시 부하에서도 loss 0
- logger drop, malformed record, RX overflow, application queue pressure와 producer overflow 0
- source별 `BENCH_TX` retransmission/failure 및 `BENCH_LINK` metric 기록
- ACK 및 key-position retry가 켜진 release 설정을 별도로 보존
- 측정 firmware, commit, period, clock Hz 근거, RF 조건 기록

Duplicate는 ACK 유실 뒤 재전송에서 발생할 수 있어 loss와 별도 항목으로 기록한다. Duplicate를 숨기거나 accepted fresh event에 포함하지 않는다.

## 4. USB endpoint와 interrupt cadence 측정

### Windows capture

1. Wireshark 설치 시 USBPcap 구성 요소를 설치한다.
2. Wireshark를 관리자 권한으로 열고 Prospector가 연결된 USBPcap interface를 선택한다.
3. Capture를 먼저 시작한 뒤 dongle을 다시 연결해 enumeration과 endpoint descriptor를 함께 기록한다.
4. 다른 USB 장치가 섞여 있으므로 `usb.bus_id`, `usb.device_address`, interrupt-IN endpoint를 확인한다.
5. Capture를 `.pcapng`로 저장한다.

Wireshark의 USB capture는 Windows host stack의 URB를 기록하며 raw bus의 개별 transaction을 기록하지 않는다. 이 제한은 [Wireshark USB capture 문서](https://wiki.wireshark.org/CaptureSetup/USB)에 명시되어 있다.

### `bInterval`

다음 명령으로 capture에 포함된 endpoint descriptor를 확인한다.

```powershell
$tshark = Join-Path $env:ProgramFiles 'Wireshark\tshark.exe'

& $tshark -r .\captures\totem-usb.pcapng `
  -Y 'usb.bInterval' `
  -T fields -E 'header=y' -E 'separator=,' -E 'quote=d' `
  -e frame.number -e usb.device_address `
  -e usb.bEndpointAddress -e usb.bEndpointAddress.direction `
  -e usb.bmAttributes.transfer -e usb.bInterval
```

Keyboard HID interrupt-IN endpoint 행에서 Full-Speed `bInterval=1`을 확인한다. Capture가 enumeration 뒤에 시작되어 descriptor가 없으면 재연결해서 다시 캡처한다. Wireshark가 제공하는 `usb.bInterval`, endpoint 및 transfer field는 [USB display-filter reference](https://www.wireshark.org/docs/dfref/u/usb.html)에서 확인할 수 있다.

### CSV export와 분석

```powershell
$tshark = Join-Path $env:ProgramFiles 'Wireshark\tshark.exe'

& $tshark -r .\captures\totem-usb.pcapng `
  -Y 'usb.transfer_type == 0x01 && usb.endpoint_address.direction == 1 && usb.urb_type == URB_COMPLETE' `
  -T fields -E 'header=y' -E 'separator=,' -E 'quote=d' -E 'occurrence=f' `
  -e frame.number -e frame.time_epoch `
  -e usb.bus_id -e usb.device_address -e usb.endpoint_address `
  -e usb.urb_type -e usb.data_len -e usbhid.data -e usb.capdata |
  Set-Content -Encoding utf8 .\captures\totem-hid-in.csv

py -3 .\tools\analyze_usbpcap.py .\captures\totem-hid-in.csv
```

TShark의 `-T fields`와 `-E` CSV 옵션은 [TShark manual](https://www.wireshark.org/docs/man-pages/tshark.html)에 정의되어 있다. `usbhid.data`와 `usb.capdata`는 각각 [USB HID](https://www.wireshark.org/docs/dfref/u/usbhid.html) 및 [USB](https://www.wireshark.org/docs/dfref/u/usb.html) field다.

여러 stream이 섞이면 bus/device/endpoint를 제한한다.

```powershell
py -3 .\tools\analyze_usbpcap.py .\captures\totem-hid-in.csv `
  --bus <BUS> --device <DEVICE> --endpoint 0x81
```

`0x81`은 예시일 뿐이다. 실제 descriptor와 CSV에서 확인한 endpoint를 사용한다. 하나의 endpoint에 여러 HID report ID가 섞이면 실제 첫 byte를 확인한 뒤 `--report-id 0xNN`을 추가한다.

분석 결과의 의미는 다음과 같다.

- `all_completion_intervals`: payload가 있는 interrupt-IN URB completion 간격
- `consecutive_repeated_payloads`: 직전 report와 payload가 같은 completion
- `payload_transition_intervals`: payload가 바뀐 USB 상태 변화 사이의 간격

USBPcap은 raw poll/NAK를 모두 보여주지 않으므로 idle 상태의 completion 수가 1,000/s가 아니어도 descriptor가 1 ms가 아니라는 뜻은 아니다. 반대로 계속 바뀌는 report가 약 1 ms completion cadence를 보여도 ESB fresh sequence가 같은 cadence로 도착했다는 증거는 아니다. Raw transaction이 필요한 경우 hardware USB analyzer를 사용한다.

## 5. Fresh radio와 반복 USB report 구별

다음 두 결과를 나란히 보관한다.

| 증거 | 확인하는 것 | 확인하지 못하는 것 |
|---|---|---|
| `BENCH_RX wire=2` source별 sequence/cadence | 실제 peripheral에서 생성되어 dongle에 도착한 fresh ESB packet | HID report 또는 host 도착 |
| USB endpoint descriptor `bInterval=1` | host service opportunity가 1 ms인 설정 | fresh ESB event rate |
| USBPcap payload completion/cadence | Windows host stack에 완료된 HID payload와 반복/변화 | raw poll 전체, ESB sequence |
| `BENCH_USB` FIFO record | 같은 dongle clock의 remote key RX→keyboard-report 함수 구간 | host 수신, 복잡한 behavior의 정확한 1:1 상관 |
| Logic analyzer의 switch/GPIO + USB | 정의한 두 지점 사이 end-to-end timing | application 내부 구간별 원인 |

Synthetic `wire=2`는 의도적으로 ZMK key event handler에 전달되지 않으며 `BENCH_USB` 또는 HID report를 만들지 않는다. Radio log의 sequence와 USBPcap HID payload 사이에는 공통 ID가 없으므로 두 파일의 timestamp를 단순히 빼서 latency를 만들면 안 된다.

## 6. 비교 실험

### 한쪽과 양쪽 동시 부하

Left-only, right-only, both 조건을 같은 duration과 RF 환경에서 실행한다. Both 결과는 총계만 쓰지 말고 source 0/1의 `accepted_fresh`, gap, duplicate, p95/p99, retransmission 및 link metric을 각각 기록한다. 서로 다른 pipe가 병렬 수신을 제공하지 않고 TDMA/CSMA도 없으므로, both 결과 악화는 반드시 실제 측정값 그대로 남긴다.

### 100,000 event loss

각 source의 한 session에서 분석기가 계산한 `wire=2 accepted_fresh`가 100,000 이상인 구간을 사용한다. 전체 sequence는 heartbeat와 실제 ZMK event도 포함하므로 source/session 전체 sequence gap 0을 요구한다. 송신을 멈춘 뒤에도 마지막 `BENCH_LINK` snapshot이 도착하도록 최소 한 full metric cycle인 1.25초 이상 기다린 다음 capture를 끝낸다. RTT line count만으로 radio receive count를 대신하지 않으며 logger-drop 경고가 있으면 재측정한다.

### Display on/off

현재 benchmark는 display-on뿐이다. Display-disabled artifact가 구현되기 전에는 비교 결과를 만들지 말고 **미구현/미측정**으로 기록한다.

### BLE baseline

기존 ZMK BLE build에는 같은 ESB sequence record가 없으므로 radio fresh cadence를 직접 동등 비교할 수 없다. 동일한 physical actuator/GPIO와 USB 측정 지점을 사용한 end-to-end 비교 또는 동일한 key actuation workload의 USB transition 비교로 범위를 명시한다.

### RF 혼잡

키보드와 dongle 위치를 고정하고 평상시/혼잡 조건을 반복한다. Final profile은 RF hopping이 꺼진 고정 channel이므로 이 시험은 hopping 시험이 아니다. Source별 `BENCH_TX attempts/retransmissions`, terminal failure, `BENCH_LINK` metric, duplicate 및 sequence loss를 함께 기록한다. `attempts`는 logical message의 모든 application retry round에 걸친 hardware transmission 합계이고 `retransmissions=attempts-1`이다. Application retry round 수와 시점을 정확히 복원한 값이라고 표현하지 않는다.

## 7. 결과 기록 표

### Build와 USB

| 항목 | firmware/commit | 방법 | 결과 | 상태 |
|---|---|---|---|---|
| 10개 Actions matrix | `3cf5bb610187ab2e84330703c28fb9a79c5c97ec` | CI run 30433480301 | 10/10 및 artifact merge 성공 | 성공 |
| Left ESB benchmark build | `3cf5bb610187ab2e84330703c28fb9a79c5c97ec` | CI | UF2 생성 | 성공 |
| Right ESB benchmark build | `3cf5bb610187ab2e84330703c28fb9a79c5c97ec` | CI | UF2 생성 | 성공 |
| Dongle ESB benchmark build, display on | `3cf5bb610187ab2e84330703c28fb9a79c5c97ec` | CI | RAM 242,760 B / 256 KiB, UF2 생성 | 성공 |
| USB Full-Speed 확인 | `3cf5bb610187ab2e84330703c28fb9a79c5c97ec` | XIAO nRF52840 USBD 코드/빌드 | Full-Speed 구조 확인, 실기 enumerate 미측정 | 구조 확인/실기 미측정 |
| HID interrupt-IN `bInterval` | `3cf5bb610187ab2e84330703c28fb9a79c5c97ec` | 병합 artifact의 동글 UF2 descriptor template decode | release/benchmark 모두 1 | 빌드 확인 |
| USB completion cadence |  | USBPcap CSV | typical / p95 / p99 | 미측정 |
| 연속 동일 payload 비율 |  | USBPcap CSV |  | 미측정 |
| Dongle RX→USB queue enter |  | `BENCH_USB`, simple 1:1 key | typical / p95 / p99 | 미측정 |
| Dongle RX→USB queue done |  | `BENCH_USB`, simple 1:1 key | typical / p95 / p99 | 미측정 |
| USB unmatched/overflow |  | benchmark log |  | 미측정 |

### ESB source별

| 조건 | source/session | `wire=2 accepted_fresh` | missing/dup/OOO | typical/p95/p99 RX | retrans/fail | link metric 0..4 | RX overflow | logger drop | 상태 |
|---|---|---:|---|---|---|---|---:|---:|---|
| Left only | 0 /  |  |  |  |  |  |  |  | 미측정 |
| Right only | 1 /  |  |  |  |  |  |  |  | 미측정 |
| Both, display on | 0 /  |  |  |  |  |  |  |  | 미측정 |
| Both, display on | 1 /  |  |  |  |  |  |  |  | 미측정 |
| Both, display off | 0 /  |  |  |  |  |  |  |  | 미구현/미측정 |
| Both, display off | 1 /  |  |  |  |  |  |  |  | 미구현/미측정 |
| RF congested, fixed channel | 0 /  |  |  |  |  |  |  |  | 미측정 |
| RF congested, fixed channel | 1 /  |  |  |  |  |  |  |  | 미측정 |

### End-to-end

| 조건 | 시작 지점 | 종료 지점 | 장비 | sample 수 | typical | p95 | p99 | 상태 |
|---|---|---|---|---:|---:|---:|---:|---|
| ESB release, display on |  |  |  |  |  |  |  | 미측정 |
| ESB benchmark, display on |  |  |  |  |  |  |  | 미측정 |
| ESB display off |  |  |  |  |  |  |  | 미구현/미측정 |
| BLE baseline |  |  |  |  |  |  |  | 미측정 |

## 8. 보고 원칙

- Build 성공과 descriptor `bInterval=1`은 각각 build/USB 설정 성공으로만 보고한다.
- 현재 10개 matrix가 실행되기 전에는 예정 artifact 이름을 build 성공으로 쓰지 않는다.
- Radio 수치가 없으면 `fresh event rate: 미측정`으로 쓴다.
- Clock 주파수 근거가 없으면 cycle 값만 기록한다.
- Source와 dongle tick 차이를 latency로 쓰지 않는다.
- Sequence 및 cadence는 source와 session을 함께 기록한다.
- Synthetic packet은 `BENCH_RX accepted=0`이 정상일 수 있으므로 분석기의 session-aware `accepted_fresh`를 사용한다.
- `BENCH_USB`는 같은 dongle clock의 내부 구간으로만 보고하고 host 수신 latency라고 쓰지 않는다.
- FIFO 1:1 조건, unmatched/overflow 및 사용한 key behavior를 `BENCH_USB` 결과와 함께 기록한다.
- Typical은 median인지 명시하고 sample/interval 수를 함께 쓴다.
- 두 half의 결과를 합산해서 한쪽 loss를 숨기지 않는다.
- `BENCH_LINK`의 마지막 snapshot은 송신 종료 후 최소 1.25초 기다려 갱신한다.
- Logger drop, malformed marker, queue pressure/overflow 또는 invalid RX가 있는 capture는 그대로 공개하고 재측정한다.
- `-EBADMSG`, `-EPROTO`, `-EMSGSIZE`와 pipe별 누적 RX overflow를 서로 다른 원인으로 기록한다.
- TDMA/CSMA가 없는 two-PTX 구조에서 동시 1K를 사전 보장하지 않는다.
- 목표값과 실측값이 다르면 실측값을 우선한다.
