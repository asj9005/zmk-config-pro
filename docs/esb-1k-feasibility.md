# Totem + Prospector ESB 1K 타당성 검토

작성 기준일: 2026-07-29

대상 브랜치: `totem-prospector` (`ee2be80d14fea84127ab55eddb833287e3871a91`)

작업 브랜치: `feature/totem-prospector-esb-1k`

## 결론

현재 XIAO nRF52840 세 대와 Prospector 어댑터를 그대로 사용하면서 ZMK BLE split을 Nordic ESB split으로 바꾸고, 동글을 USB Full-Speed HID 1 ms polling 장치로 만드는 코드 구조는 성립한다. `badjeff/zmk-feature-split-esb`는 ZMK 0.4/Zephyr 4.1용 코드와 XIAO nRF52840가 사용하는 Nordic ESB 드라이버를 제공하므로 새 RF 프로토콜을 처음부터 만들 필요는 없다.

다만 upstream 모듈을 현재 저장소에 설정만 추가하여 그대로 쓰는 것은 성공 기준을 만족하지 않는다. 확인된 차단점은 다음과 같다.

- Prospector `feat/new-status-screens`는 BLE central 상태 observer와 BLE output widget을 무조건 컴파일한다. `CONFIG_ZMK_BLE=n`인 ESB-only 동글은 최소 호환 패치 없이는 빌드되지 않는다.
- ESB 모듈의 wire peripheral ID는 1/2인데 ZMK와 Prospector 내부 배열은 0/1 source index를 기대한다. 그대로 두면 오른쪽(ID 2) 배터리 상태가 범위를 벗어난다.
- upstream의 `msg_id`는 송신 큐 내부 메타데이터이며 실제 ESB payload에서 제거된다. 따라서 upstream 코드는 그대로는 fresh-event sequence, sequence gap 및 source timestamp를 central에서 관측할 수 없다.
- upstream central은 연결 상태를 항상 `ALL_CONNECTED`로 보고하고 사용 가능한 source를 0 하나만 반환한다. 실제 좌우 연결 표시에는 heartbeat/last-seen 기반 peer 상태가 필요하다.
- 두 half는 서로 다른 pipe를 사용해도 같은 RF channel과 하나의 PRX radio를 공유한다. TDMA/CSMA가 없으므로 양쪽 동시 1 kHz 생성이 양쪽 각각 1 kHz 수신이나 무손실을 보장하지 않는다.
- source tick과 dongle tick은 서로 동기화되지 않으므로 단순 timestamp 차감만으로 one-way latency를 실측할 수 없다. 동기화 왕복 측정 또는 외부 GPIO/logic analyzer가 필요하다.

따라서 upstream ESB 모듈을 기반으로 하되, 위 항목만 고치는 작은 호환성 overlay를 별도로 유지한다. ZMK 전체를 복사하거나 대규모 fork하지 않는다. 현재 작업 브랜치의 10개 Actions matrix 항목은 아직 **대기/미검증**이며, 하드웨어에서 USB, radio latency, loss, 화면 및 battery를 계측하지 않았다. 빌드 성공만으로 1K fresh-event 달성을 주장하지 않는다.

## 1. 현재 revision과 기준 빌드

변경 전 기준 GitHub Actions run은 [#221 / run 29417265157](https://github.com/asj9005/zmk-config-pro/actions/runs/29417265157)이며, 2026-07-15에 다음 네 빌드가 모두 성공했다.

- `totem_left`
- `totem_right`
- `totem_dongle prospector_adapter`
- `settings_reset`

이 run은 `totem-prospector` 기준선만 검증한다. 현재 작업 브랜치의 BLE 회귀 빌드나 ESB release/benchmark 빌드를 검증한 결과가 아니다.

최종 `config/west.yml`과 reusable workflow는 다음 exact commit을 사용한다.

| 구성요소 | 고정 revision |
|---|---|
| ZMK | `904c9aec8822d79149d42c8a9a77e8828eb08f5a` |
| Zephyr (`zmkfirmware/zephyr`) | `9df4b12b5af3438a8b9d7a33780dc3b3b2f516c1` |
| `badjeff/zmk-feature-split-esb` | `1f4cd4558bb9e0626ec2507f334f239862af859d` |
| `badjeff/sdk-nrf` | `9b3d2623fdcd9c0fd0284f860beea924568c9826` |
| `nrfconnect/sdk-nrfxlib` | `dfadf17305d8f000eda9aa74a5b9ff1c5647a23e` |
| `nrfconnect/sdk-mbedtls` | `c5115abac477249fab42e61368b8f87c3c9265e1` |
| `nrfconnect/sdk-oberon-psa-crypto` | `d682b30a4498ecbaa8992e909b8d8c31f4988956` |
| Prospector | `ed98221f3b52b7066dbb10ba3af8a29150b93a5a` |
| zmk-tri-state | `2007896c6d5bfb519e8babccf8633841c5647d8b` |

Zephyr는 ZMK import의 moving branch에 맡기지 않고 main manifest에서 exact SHA로 override한다. `.github/workflows/build.yml`의 reusable workflow도 같은 ZMK SHA를 사용한다. `badjeff/zmk-config`의 `esb-shield-only` 예제 `594a7d5b962d252036c8391193fda3d3da659573`은 참고 자료일 뿐 build dependency가 아니다.

## 2. 현재 split transport 정의와 코드 경로

저장소 쪽 역할 정의는 다음 파일에 있다.

- `config/boards/shields/totem/Kconfig.defconfig`: left/right/dongle의 `ZMK_SPLIT`, central/peripheral 및 USB 기본값
- `config/boards/shields/totem/totem_left.conf`
- `config/boards/shields/totem/totem_right.conf`
- `config/boards/shields/totem/totem_dongle.conf`: central의 BLE peripheral count와 Prospector 설정
- `config/totem.conf`: 현재 BLE PHY, sleep, battery proxy 및 pointing 공통 설정
- `config/totem_esb.conf`: 공통 ESB, ACK/CRC/queue 및 retry 설정
- `config/totem_esb_left.conf`, `config/totem_esb_right.conf`, `config/totem_esb_dongle.conf`: 역할별 설정
- `build.yaml`: BLE, reset, ESB release 및 ESB benchmark shield 조합

기존 프로필의 데이터 경로는 ZMK의 `app/src/split/bluetooth/central.c`, `peripheral.c` 및 공통 split event serialization을 거친다. 새 프로파일은 기존 shield/키맵을 복사하지 않고 auxiliary shield/config fragment로 `CONFIG_ZMK_SPLIT_BLE=n`, `CONFIG_ZMK_SPLIT_ESB=y`를 덮어쓴다. ESB 경로는 pinned `zmk-feature-split-esb`의 `src/split/esb/{peripheral,central,common,app_esb}.c`에 이 저장소의 narrow compatibility overlay를 적용한다. 기존 BLE 빌드 항목은 그대로 남긴다.

## 3. 현재 USB HID endpoint polling interval

ZMK `904c9aec...`의 `app/Kconfig`는 `ZMK_USB`일 때 `CONFIG_USB_HID_POLL_INTERVAL_MS` 기본값을 1로 둔다. Zephyr HID device core는 이 값을 Full-Speed interrupt endpoint descriptor의 `bInterval`에 넣는다. 따라서 현재 Prospector 동글도 설정상 1 ms가 기본값이지만, 기준 run의 산출물 descriptor를 별도로 추출해 확인한 기록은 없다.

ESB 동글 설정에는 `CONFIG_USB_HID_POLL_INTERVAL_MS=1`을 명시한다. 최종 검증은 빌드된 USB descriptor의 `bInterval=1`과 실제 USBPcap interrupt cadence를 분리하여 기록한다. 이 항목은 USB host가 1 ms마다 poll할 수 있음을 뜻할 뿐, 새 radio event가 1 ms마다 도착한다는 뜻은 아니다.

## 4. 현재 ZMK/Zephyr와 ESB 모듈 호환성

`zmk-feature-split-esb`의 현재 코드는 ZMK 0.4 및 Zephyr 4.1을 대상으로 하며, nRF52840 ESB를 2 Mbps/fast-ramp-up으로 설정한다. XIAO BLE board도 nRF52840이므로 radio 하드웨어 경로는 맞는다. tri-state와 기존 hold-tap/sticky/combo/mouse behavior는 central에서 처리되는 key position event 위에 있으므로 transport 교체 자체로 키맵을 수정할 이유가 없다.

호환성 판정은 다음과 같다.

| 조합 | 판정 | 근거/조치 |
|---|---|---|
| ZMK 0.4 + Zephyr 4.1 | 조건부 호환 | ESB 모듈의 목표 조합. Exact SHA는 고정했으나 현 branch CI 결과는 대기 |
| XIAO BLE / nRF52840 | 호환 후보 | Nordic ESB 지원 SoC. 예제의 좌우는 nice_nano이므로 XIAO 좌우 빌드 검증은 별도 필요 |
| tri-state | 구조상 호환 | keymap/behavior 파일을 변경하지 않고 회귀 빌드 |
| ZMK pointing/mouse key | 구조상 호환 | central의 기존 behavior 처리 유지. queue/stack 크기 검증 필요 |
| Prospector 새 화면 | 조건부 호환 | BLE observer/output 및 source index overlay 구현. CI/실기 검증 대기 |
| GitHub Actions | 조건부 호환 | 모든 dependency SHA pin 완료. 현재 10개 matrix 빌드 검증 대기 |

## 5. 패치된 NCS와 nrfxlib가 필요한 이유

ESB 모듈 README는 ZMK 0.4/Zephyr 4.1 조합에서 다음 두 project를 요구한다.

- `badjeff/sdk-nrf`의 `v3.1-branch+zmk-fixes`
- `nrfconnect/sdk-nrfxlib`의 대응 `v3.1-branch`

이 조합은 NCS 3.1 계열 nrfx ESB library와 Zephyr 4.1 CMake/Kconfig 검증을 맞추기 위한 것이다. 일반 ZMK manifest의 Nordic project 조합만 사용하면 ESB library/CMake 구성이 일치하지 않는다. 패치된 `sdk-nrf`의 security module은 BLE rollback 빌드에서도 Zephyr module로 발견되므로 NCS 3.1.1에 대응하는 `sdk-mbedtls`와 `sdk-oberon-psa-crypto`도 exact SHA로 고정한다. Oberon project가 없으면 `OBERON_PSA_CORE_PATH`가 비어 BLE 빌드의 crypto source가 filesystem root에서 조회된다. `badjeff/zmk` fork는 ESB 모듈 자체의 필수 의존성이 아니므로, upstream ZMK로 빌드가 성립하면 추가하지 않는다.

Compatibility overlay가 복사하는 upstream 파일의 SPDX 고지는 유지한다. Nordic-derived 파일은 `LicenseRef-Nordic-5-Clause`, ZMK-derived 파일은 MIT이며, 새 Totem 전용 파일은 MIT다. 정확한 overlay 범위와 라이선스 경계는 `compat/README.md`에 기록한다.

## 6. 두 peripheral을 하나의 central이 처리하는 방식

`CONFIG_ESB_PIPE_COUNT=3`이다. Pipe 0은 예약하고, 왼쪽 wire ID 1은 pipe 1, 오른쪽 wire ID 2는 pipe 2를 사용한다. 세 firmware는 하나의 공통 DTSI에 정의한 base address와 세 prefix를 공유한다. Prospector central은 pipe와 envelope source가 일치하는지 확인한 뒤 wire ID 1/2를 ZMK logical source 0/1로 바꾸고 하나의 ZMK event stream으로 병합한다.

Pipe 1/2는 양방향으로 독립 송신하는 radio link가 아니다. Half→dongle event는 PTX uplink이며, dongle→half command는 같은 pipe의 PRX ACK payload다. PRX는 command를 선제 송신할 수 없고 해당 half가 다음 event 또는 heartbeat를 보낼 때만 ACK payload를 전달할 기회가 생긴다. SDK가 PRX ACK payload별 completion ID를 제공하지 않으므로 `CONFIG_ZMK_SPLIT_ESB_RETRY_CMD=0`이며 별도 command application retry는 없다.

Nordic PRX의 hardware ACK-payload FIFO는 pipe별 selective flush를 제공하지 않는다. 이미 hardware FIFO에 들어간 command의 대상 half가 이를 소비하기 전에 꺼지면 그 entry를 source별로 회수할 수 없고, 다른 half의 reverse command가 지연될 수 있다. 이는 half→dongle key uplink 손실과는 다른 알려진 역방향 queue 한계이며 실제 장치에서 재현·영향을 확인해야 한다.

upstream은 wire ID와 ZMK logical source index를 동일하게 사용하지만, ZMK/Prospector 배열은 0부터 시작한다. Compatibility overlay는 다음을 보장한다.

- peripheral → central: wire ID 1/2를 ZMK logical source 0/1로 변환
- central → peripheral command: logical source 0/1을 wire ID 1/2로 변환
- available sources: 0과 1을 모두 반환
- battery 및 Prospector widget도 logical source 0/1만 사용

이렇게 해야 peripheral count를 실제 개수 2로 둘 수 있고 오른쪽 배터리의 out-of-bounds를 막을 수 있다.

두 pipe는 동일 RF channel과 동글의 단일 PRX radio를 공유한다. 동글은 동시에 두 packet을 복조하지 못하며 현재 protocol에는 TDMA, CSMA 또는 중앙 스케줄링이 없다. 왼쪽 500 µs와 오른쪽 800 µs의 비대칭 hardware retry delay는 결정론적인 재충돌 가능성을 줄이기 위한 완화책일 뿐, 양쪽 동시 1 kHz fresh delivery를 보장하지 않는다.

## 7. ACK, 재전송, sequence 처리

최종 선택 설정은 다음과 같다.

| 항목 | 설정/동작 |
|---|---|
| ESB ACK | 활성화 |
| Hardware retransmit count | 3; 최초 송신을 포함하면 한 round에 최대 4번 전송 |
| Hardware retry delay | left 500 µs, right 800 µs |
| Application retry | key/sensor/battery 3, heartbeat 1, input/synthetic 0, PRX command 0 |
| Application checksum | envelope CRC32 postfix |
| Nordic radio CRC | ESB 기본 CRC16 |
| RF channel hopping | 비활성화 |
| Key recovery | repeated press auto-heal, session 변경/peer timeout 시 held-key release |

Key/sensor/battery는 한 hardware round가 완전히 실패하면 동일 payload를 순서를 바꾸지 않고 최대 세 번 더 시도한다. 따라서 설정상 최댓값은 네 round, round당 최대 네 번의 radio transmission이다. `BENCH_TX attempts`는 이 message의 모든 round에서 Nordic callback이 보고한 hardware transmission 횟수를 합한 terminal 값이고, `retransmissions=attempts-1`이다. 이는 application retry round의 개수와 각 시각을 따로 보여 주지는 않는다. Synthetic `wire=2`에는 application retry가 없으므로 key-position retry 3의 신뢰성을 대신 검증하지 않는다.

Upstream hopping은 PTX와 PRX의 channel 상태를 동기화하지 않는다. 한쪽만 channel을 바꾸면 link를 잃을 수 있으므로 final profile은 `CONFIG_ZMK_SPLIT_ESB_RF_CH_HOP=n`이다. RF 혼잡 시험은 고정 channel에서 retry/loss를 측정하는 시험이지 hopping 성능 시험이 아니다.

Compatibility protocol은 각 uplink packet에 wire source, 32-bit random boot session, monotonic sequence, wire/event type, source tick 및 event body를 넣는다. Half는 부팅 때 nRF52840 hardware entropy로 non-zero session ID를 만든다. Dongle은 session이 바뀌면 해당 source의 sequence state를 재설정하고 아직 눌린 것으로 추적한 key를 release한다. Peer timeout 때도 held key를 release하고 다음 packet을 새 sequence 기준으로 받는다. 이 경로는 half 재부팅 후 stale-sequence 거부와 stuck release를 완화한다. Random session ID는 충돌 가능성이 있는 32-bit 식별자이며 인증·암호화·replay 방지는 아니다.

`BENCH_RX accepted=1`은 sequence가 fresh이고 `wire=0` ZMK event가 active ESB transport로 전달되었다는 뜻이다. Heartbeat와 synthetic `wire=2`는 정상 수신돼도 ZMK event handler에 전달하지 않으므로 `accepted=0`이다. 분석기는 synthetic fresh count를 `accepted` field가 아니라 session별 sequence 규칙으로 계산한다.

Heartbeat는 누적 link counter를 metric 0부터 4까지 순환해 전달한다.

| metric | 의미 |
|---:|---|
| 0 | terminal TX message 수 |
| 1 | 모든 application round를 합친 hardware TX attempt 수 |
| 2 | retry budget을 모두 소진한 terminal TX failure 수 |
| 3 | ESB application message queue pressure 횟수 |
| 4 | producer TX ring overflow 횟수 |

Central은 이를 `BENCH_LINK source=... session=... metric=... value=...`로 노출한다. 한 heartbeat에는 metric 하나만 들어가므로 기본 250 ms heartbeat에서는 같은 metric이 약 1.25초마다 갱신된다.

Wire parser는 오류를 구분한다. CRC32 불일치는 `-EBADMSG`, magic prefix 불일치는 `-EPROTO`, envelope size/type 불일치는 `-EMSGSIZE`다. RX ring overflow는 `BENCH_RX_OVERFLOW`의 누적 count로 별도 기록한다. 이 값들은 application envelope 검사이며 Nordic radio CRC 자체의 hardware reject count를 직접 보여 주지는 않는다.

신뢰성이 필요한 key press/release에는 ACK와 retry를 유지한다. Benchmark 수치를 위해 ACK를 끄지 않는다. 모든 wire payload는 `CONFIG_ESB_MAX_PAYLOAD_LENGTH=48` 이하여야 하며 `BUILD_ASSERT`로 검사한다.

## 8. Prospector 화면 task의 영향

Prospector `ed98221f...`는 adapter가 활성화되면 `src/split/bluetooth/central_status_changed_observer.c`를 무조건 컴파일한다. 이 파일은 Zephyr Bluetooth와 ZMK BLE API를 사용한다. Operator `output.c`도 BLE profile/event API를 무조건 사용하며, battery widget은 `ZMK_SPLIT_BLE_PERIPHERAL_COUNT`와 event source 직접 indexing을 가정한다. ESB-only compatibility overlay는 다음을 적용한다.

- BLE central observer를 transport-agnostic split peer observer로 교체
- Operator output widget을 USB-only 상태로 컴파일 가능하게 함
- source 0/1과 peripheral count 2 사용
- ESB heartbeat/last-seen timeout으로 실제 left/right 상태 event 생성

Prospector listener는 연속된 source event를 coalesce할 수 있다. Peer transition 또는 battery event가 생기면 100 ms 뒤 authoritative source 0/1 상태를 500 ms 간격으로 한 번씩 재게시하는 bounded full pass를 사용한다. Link가 안정된 동안 500 ms마다 무한 refresh하지 않는다.

화면은 release와 현재 benchmark 동글 firmware 모두에 유지한다. Display와 LVGL flush thread priority는 10으로 두어 radio IRQ 및 USB 경로보다 낮게 실행하도록 구성한다. 화면이 queue latency에 미치는 영향은 코드만으로 수치화하지 않는다. 현재 `build.yaml`에는 display-disabled artifact가 없으므로 display on/off 비교는 **미구현/미측정**이다.

Benchmark는 release와 메모리·timing 조건도 다르다. Deferred log buffer 8 KiB, RTT up buffer 4 KiB, dongle pending-USB FIFO 128개를 추가하고 per-packet logging을 수행한다. RTT backend는 DROP mode이므로 consumer가 따라오지 못해 log가 하나라도 빠진 capture로 packet loss 0을 주장할 수 없다. Benchmark build의 RAM 적합성 및 logging 부하는 현재 10개 CI와 실제 장치에서 아직 검증되지 않았다.

## 9. 배터리 상태 전달

ESB 모듈은 BLE 이름을 가진 ZMK battery Kconfig를 `!ZMK_SPLIT_BLE`일 때도 제공하며, peripheral battery event를 serialize하고 central의 표준 `zmk_peripheral_battery_state_changed` 처리기로 전달한다. 이름만 보고 이 설정을 제거하면 안 된다.

Wire 전달 자체는 지원되며 compatibility overlay가 wire ID 1/2를 logical source 0/1로 바꾼다. Peer transition 또는 battery event 뒤 bounded full pass가 두 source의 표시 상태를 한 번씩 다시 게시한다. 이 코드 경로의 CI와 실제 배터리 값/화면 표시는 아직 **미검증/미측정**이다.

## 10. USB 1K와 fresh radio 1K의 차이

- **USB 1K polling**: Full-Speed HID interrupt endpoint의 `bInterval=1`; host가 최대 1 ms 간격으로 IN transaction을 요청할 수 있다.
- **fresh radio event delivery**: 각 하프에서 새로 만들어진 sequence가 central에 도착한 간격과 loss를 말한다.
- **end-to-end latency**: matrix scan/debounce → peripheral queue → ESB airtime/retry → central queue/behavior → USB report → host 수신까지의 시간이다.

동일 HID report를 매 USB poll에 반복하는 것은 fresh-event 1K가 아니다. 최종 release에서 1K급이라고 말하려면 peripheral별 sequence, central RX inter-arrival, 양쪽 동시 부하 loss 및 p95/p99를 실제 장비로 기록해야 한다. 목표값은 구조적 목표이며 실측값이 아니다.

## 11. 암호화·인증과 보안 한계

Nordic ESB의 address, CRC 및 ACK는 암호화나 peer 인증이 아니다. 이번 transport에는 기밀성, 송신자 인증 및 replay 방지가 없다. 같은 address를 아는 근처 장치는 packet을 관찰하거나 위조할 수 있다. 생성하는 address는 다른 기본 예제와의 우발적 충돌을 줄이는 식별자일 뿐 비밀키가 아니다. 이 제한을 README와 flash 문서에 명시한다.

## 12. 양쪽 동시 입력의 1 ms급 처리 검증

두 half 사이에는 TDMA/CSMA가 없고 동글은 한 순간에 한 packet만 수신할 수 있다. 비대칭 retry delay는 충돌 완화책일 뿐이다. 따라서 코드나 airtime 계산만으로 양쪽 동시 입력 성능을 확정하지 않고 benchmark firmware와 도구로 최소 다음을 분리 기록한다.

1. source 0/1의 session별 monotonic sequence 및 수신 packet count
2. `BENCH_RX`의 `session`, `wire=0`, `accepted=1`을 이용한 실제 입력 전달 구분과 분석기의 session-aware `accepted_fresh`
3. source별 `BENCH_TX` attempt/retransmission/failure와 `BENCH_LINK` 누적 metric
4. CRC/prefix/size 오류, RX overflow 및 producer/application queue pressure
5. dongle RX inter-arrival과 USB report enqueue 시각
6. 한쪽 단독, 양쪽 동시, 현재 display-on, RF 혼잡 조건
7. 각 조건 100,000 fresh synthetic event 이상에서 loss
8. typical, p95, p99 및 최대 지연

source와 dongle clock이 동기화되지 않은 상태의 timestamp 차이는 latency로 보고하지 않는다. one-way latency는 GPIO/logic analyzer 또는 왕복 clock offset 보정으로 측정하고, USBPcap/Wireshark로 USB interrupt interval을 별도로 검증한다.

## 선택한 구현 경계

- 기존 `totem_left`, `totem_right`, `totem_dongle prospector_adapter`, `settings_reset` 빌드는 유지한다.
- ESB는 auxiliary shield/config fragment로 추가하여 기존 `config/totem.keymap`을 수정하지 않는다.
- 새 address는 공통 DTSI 한 곳에만 둔다.
- Release와 현재 benchmark는 display enabled다. Display-disabled 변형은 아직 구현하지 않는다.
- 외부 project와 reusable workflow는 exact SHA로 고정하고 각 upstream license를 보존한다.
- `build.yaml`의 10개 항목은 BLE 3, reset 1, ESB release 3, ESB benchmark 3이다. 현재 branch CI 결과는 대기/미검증이다.
- 실제 하드웨어 계측이 끝나기 전 결과 표의 fresh-event rate, loss 및 end-to-end latency는 `미측정`으로 남긴다.

## 참고 구현

- <https://github.com/badjeff/zmk-feature-split-esb/tree/1f4cd4558bb9e0626ec2507f334f239862af859d>
- <https://github.com/badjeff/zmk-config/tree/594a7d5b962d252036c8391193fda3d3da659573/boards/shields/donki36>
- <https://github.com/carrefinho/prospector-zmk-module/tree/ed98221f3b52b7066dbb10ba3af8a29150b93a5a>
- <https://github.com/zmkfirmware/zmk/tree/904c9aec8822d79149d42c8a9a77e8828eb08f5a>
