# Totem ESB 1K 계측 도구

두 스크립트는 Python 표준 라이브러리만 사용한다. Windows에서는 저장소 루트에서 `py -3`로 실행할 수 있다.

## ESB RTT 로그

```powershell
py -3 .\tools\analyze_esb_benchmark.py .\captures\esb-dongle.log
```

이 명령은 주파수를 추정하지 않고 source/session별 sequence loss와 cycle 단위 도착 간격을 계산한다. 빌드에서 확인한 `k_cycle_get_32()` 주파수를 알고 있을 때만 다음처럼 지정한다.

```powershell
py -3 .\tools\analyze_esb_benchmark.py .\captures\esb-dongle.log `
  --dongle-clock-hz <DONGLE_HZ> `
  --source-clock-hz <PERIPHERAL_HZ>
```

`--clock-hz <HZ>`는 두 값을 한 번에 지정한다. 주파수가 확실하지 않으면 옵션을 생략해야 한다. 결과에서 `typical`은 중앙값(p50)이다.

분석 범위는 다음과 같다.

- 모든 `BENCH_RX` packet의 source/session별 sequence gap, duplicate, out-of-order
- `wire=2` synthetic fresh event의 session-aware `accepted_fresh`, source 생성 간격과 dongle 도착 간격
- 실제 key-position event(`wire=0 event=0`)의 press/release 수
- `BENCH_USB`의 source/session/sequence identity와 같은-dongle-clock RX→USB queue enter/done 구간
- `BENCH_USB_UNMATCHED`와 `BENCH_USB_QUEUE_OVERFLOW`
- Source가 포함된 `BENCH_TX` terminal 성공·실패, hardware attempt 및 `attempts-1` retransmission
- `BENCH_LINK` metric 0~4의 source/session별 마지막 누적값
- Invalid RX errno, pipe별 누적 RX overflow, TX queue pressure 및 logger-drop 메시지

JSON이 필요하면 `--json`을 붙인다. Dongle RTT에는 `BENCH_RX`, `BENCH_LINK`, `BENCH_USB`와 RX 오류/overflow가 기록되고, left/right RTT에는 각 source의 `BENCH_TX`가 기록된다. 파일을 따로 분석하거나 단순히 결합해 count를 볼 수 있지만, 서로 다른 장치의 행 순서와 tick을 시간축으로 합치면 안 된다. `msg`는 source-local 16-bit ID로 장시간 capture에서 재사용되므로 unique packet count로 쓰지 않는다.

`BENCH_RX accepted=1`은 fresh `wire=0` event가 active ZMK transport로 전달됐다는 뜻이다. 정상 heartbeat와 synthetic `wire=2`도 `accepted=0`이므로 스크립트는 이 field가 아니라 source/session sequence로 synthetic `accepted_fresh`를 계산한다.

`BENCH_LINK` metric은 다음과 같다.

| metric | 의미 |
|---:|---|
| 0 | terminal TX message 수 |
| 1 | application retry round 전체의 hardware TX attempt 수 |
| 2 | terminal TX failure 수 |
| 3 | ESB application queue pressure 횟수 |
| 4 | producer TX ring overflow 횟수 |

Heartbeat는 기본 250 ms마다 metric 하나를 round-robin으로 보내므로 마지막 snapshot은 최대 약 1.25초 늦다. 부하 생성을 멈춘 뒤 최소 1.25초 기다리고 capture를 종료한다.

Invalid RX의 주요 errno는 CRC32 mismatch `-EBADMSG` (`-74`), magic prefix mismatch `-EPROTO` (`-71`), payload size/type mismatch `-EMSGSIZE` (`-90`)다. `BENCH_RX_OVERFLOW`는 parser 오류와 별개인 pipe별 누적 RX ring overflow다.

중요: peripheral의 `source_tick`과 dongle의 `dongle_tick`은 동기화된 시계가 아니다. `BENCH_TX source_tick`도 terminal callback 시각일 뿐 packet 생성 시각이 아니다. 스크립트는 서로 다른 장치 tick의 차이를 one-way latency로 계산하지 않는다.

`BENCH_USB`의 `rx_tick`, `queue_enter_tick`, `queue_done_tick`은 모두 dongle clock이므로 서로의 차이를 계산할 수 있다. 다만 firmware는 수신 key event와 keyboard-report 함수 호출을 FIFO로 연결한다. 일반 key 한 개가 press/report, release/report로 1:1 대응하는 단순 테스트에서만 사용한다. Hold-tap, layer, combo, sticky key, local event 또는 report coalescing이 들어가면 unmatched 또는 잘못 정렬된 표본이 생길 수 있다. `wire=2` synthetic packet은 HID report를 만들지 않는다.

Benchmark firmware는 deferred log buffer 8 KiB, RTT up buffer 4 KiB와 DROP mode를 사용한다. Logger drop, malformed benchmark marker, RX overflow, application queue pressure 또는 producer overflow가 있으면 loss 0 결과로 사용할 수 없다. Per-packet logging과 추가 RAM 때문에 benchmark timing은 release timing과 동일하지 않다. 현재 benchmark는 display-on이며 display-disabled artifact는 구현되지 않았다.

Left와 right는 같은 fixed RF channel과 하나의 dongle PRX를 공유하고 TDMA/CSMA를 사용하지 않는다. Hardware retry delay는 left 500 µs/right 800 µs로 다르지만 동시 1 kHz를 보장하지 않는다. RF hopping은 꺼져 있으므로 analyzer 결과도 source별 실제 동시부하와 고정-channel 혼잡 결과로 해석한다.

## USBPcap/TShark CSV

Wireshark에서 저장한 `.pcapng`를 TShark CSV로 변환한 뒤 실행한다.

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

여러 장치나 endpoint가 섞였으면 필터를 지정한다.

```powershell
py -3 .\tools\analyze_usbpcap.py .\captures\totem-hid-in.csv `
  --bus 1 --device 7 --endpoint 0x81
```

같은 endpoint에 keyboard, mouse 등 여러 HID report ID가 섞이면 첫 payload byte를 Wireshark에서 확인한 뒤 `--report-id 0xNN`으로 하나만 선택한다. report ID를 추측해서 넣으면 안 된다.

스크립트가 출력하는 항목은 다음과 같다.

- payload가 있는 interrupt-IN completion 간격의 typical/p95/p99
- 직전 payload와 동일한 연속 report 수와 비율
- payload가 바뀐 transition의 수와 간격

USB HID payload에는 ESB sequence가 없다. 따라서 `payload transition`은 USB 상태 변화일 뿐, fresh radio event 증거가 아니다. 반대로 같은 payload가 반복되었다는 사실만으로 radio event가 없었다고 단정할 수도 없다.

USBPcap은 raw USB transaction이 아니라 Windows host stack의 URB를 캡처한다. `bInterval=1`은 endpoint descriptor에서 별도로 확인하고, 실제 bus poll 자체를 정확히 관측해야 하면 hardware USB analyzer를 사용한다.

전체 절차와 결과 기록 표는 [`docs/esb-1k-benchmark.md`](../docs/esb-1k-benchmark.md)를 따른다.
