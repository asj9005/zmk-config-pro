# Totem + Prospector ESB 1K 타당성 검토

작성 기준일: 2026-07-29  
대상 브랜치: `totem-prospector` (`ee2be80d14fea84127ab55eddb833287e3871a91`)  
작업 브랜치: `feature/totem-prospector-esb-1k`

## 결론

현재 XIAO nRF52840 세 대와 Prospector 어댑터를 그대로 사용하면서 ZMK BLE split을 Nordic ESB split으로 바꾸고, 동글을 USB Full-Speed HID 1 ms polling 장치로 만드는 것은 구조적으로 가능하다. `badjeff/zmk-feature-split-esb`는 ZMK 0.4/Zephyr 4.1용 코드와 XIAO nRF52840가 사용하는 Nordic ESB 드라이버를 제공하므로 새 RF 프로토콜을 처음부터 만들 필요는 없다.

다만 upstream 모듈을 현재 저장소에 설정만 추가하여 그대로 쓰는 것은 성공 기준을 만족하지 않는다. 확인된 차단점은 다음과 같다.

- Prospector `feat/new-status-screens`는 BLE central 상태 observer와 BLE output widget을 무조건 컴파일한다. `CONFIG_ZMK_BLE=n`인 ESB-only 동글은 최소 호환 패치 없이는 빌드되지 않는다.
- ESB 모듈의 wire peripheral ID는 1/2인데 ZMK와 Prospector 내부 배열은 0/1 source index를 기대한다. 그대로 두면 오른쪽(ID 2) 배터리 상태가 범위를 벗어난다.
- upstream의 `msg_id`는 송신 큐 내부 메타데이터이며 실제 ESB payload에서 제거된다. 따라서 현재 코드는 fresh-event sequence, sequence gap 및 source timestamp를 central에서 관측할 수 없다.
- upstream central은 연결 상태를 항상 `ALL_CONNECTED`로 보고하고 사용 가능한 source를 0 하나만 반환한다. 실제 좌우 연결 표시에는 heartbeat/last-seen 기반 peer 상태가 필요하다.
- source tick과 dongle tick은 서로 동기화되지 않으므로 단순 timestamp 차감만으로 one-way latency를 실측할 수 없다. 동기화 왕복 측정 또는 외부 GPIO/logic analyzer가 필요하다.

따라서 upstream ESB 모듈을 기반으로 하되, 위 항목만 고치는 작은 호환성 패치/모듈을 별도로 유지한다. ZMK 전체를 복사하거나 대규모 fork하지 않는다. 빌드 성공만으로 1K fresh-event 달성을 주장하지 않으며, 하드웨어 계측 전까지 radio latency와 loss는 **미측정**으로 표시한다.

## 1. 현재 revision과 기준 빌드

기준 GitHub Actions run은 [#221 / run 29417265157](https://github.com/asj9005/zmk-config-pro/actions/runs/29417265157)이며, 2026-07-15에 다음 네 빌드가 모두 성공했다.

- `totem_left`
- `totem_right`
- `totem_dongle prospector_adapter`
- `settings_reset`

그 run에서 실제로 해석된 revision은 다음과 같다.

| 구성요소 | 현재 기준 revision |
|---|---|
| 저장소 | `ee2be80d14fea84127ab55eddb833287e3871a91` |
| ZMK | `904c9aec8822d79149d42c8a9a77e8828eb08f5a` |
| Zephyr (`zmkfirmware/zephyr`, `v4.1.0+zmk-fixes`) | `9df4b12b5af3438a8b9d7a33780dc3b3b2f516c1` |
| Prospector `feat/new-status-screens` | `ed98221f3b52b7066dbb10ba3af8a29150b93a5a` |
| zmk-tri-state | `2007896c6d5bfb519e8babccf8633841c5647d8b` |

현재 `config/west.yml`은 ZMK, Prospector 및 tri-state에 움직이는 branch 이름을 사용한다. ESB 작업에서는 먼저 호환 조합으로 빌드를 성립시킨 뒤 최종 manifest와 reusable workflow를 검증된 commit SHA로 고정한다.

검토 시점의 ESB 후보 revision은 다음과 같다.

| 구성요소 | 검토 revision |
|---|---|
| `badjeff/zmk-feature-split-esb` | `1f4cd4558bb9e0626ec2507f334f239862af859d` |
| `badjeff/sdk-nrf` (`v3.1-branch+zmk-fixes`) | `9b3d2623fdcd9c0fd0284f860beea924568c9826` |
| `nrfconnect/sdk-nrfxlib` (`v3.1-branch`) | `dfadf17305d8f000eda9aa74a5b9ff1c5647a23e` |
| `badjeff/zmk-config` 예제 (`esb-shield-only`) | `594a7d5b962d252036c8391193fda3d3da659573` |

## 2. 현재 split transport 정의와 코드 경로

저장소 쪽 역할 정의는 다음 파일에 있다.

- `config/boards/shields/totem/Kconfig.defconfig`: left/right/dongle의 `ZMK_SPLIT`, central/peripheral 및 USB 기본값
- `config/boards/shields/totem/totem_left.conf`
- `config/boards/shields/totem/totem_right.conf`
- `config/boards/shields/totem/totem_dongle.conf`: central의 BLE peripheral count와 Prospector 설정
- `config/totem.conf`: 현재 BLE PHY, sleep, battery proxy 및 pointing 공통 설정
- `build.yaml`: 실제 shield 조합

현재 데이터 경로는 ZMK의 `app/src/split/bluetooth/central.c`, `peripheral.c` 및 공통 split event serialization을 거친다. 새 프로파일은 기존 shield/키맵을 복사하지 않고 auxiliary shield/config fragment로 `CONFIG_ZMK_SPLIT_BLE=n`, `CONFIG_ZMK_SPLIT_ESB=y`를 덮어쓴다. 기존 BLE 빌드 항목은 그대로 남긴다.

## 3. 현재 USB HID endpoint polling interval

ZMK `904c9aec...`의 `app/Kconfig`는 `ZMK_USB`일 때 `CONFIG_USB_HID_POLL_INTERVAL_MS` 기본값을 1로 둔다. Zephyr HID device core는 이 값을 Full-Speed interrupt endpoint descriptor의 `bInterval`에 넣는다. 따라서 현재 Prospector 동글도 설정상 1 ms가 기본값이지만, 기준 run의 산출물 descriptor를 별도로 추출해 확인한 기록은 없다.

ESB 동글 설정에는 `CONFIG_USB_HID_POLL_INTERVAL_MS=1`을 명시한다. 최종 검증은 빌드된 USB descriptor의 `bInterval=1`과 실제 USBPcap interrupt cadence를 분리하여 기록한다. 이 항목은 USB host가 1 ms마다 poll할 수 있음을 뜻할 뿐, 새 radio event가 1 ms마다 도착한다는 뜻은 아니다.

## 4. 현재 ZMK/Zephyr와 ESB 모듈 호환성

`zmk-feature-split-esb`의 현재 코드는 ZMK 0.4 및 Zephyr 4.1을 대상으로 하며, nRF52840 ESB를 2 Mbps/fast-ramp-up으로 설정한다. XIAO BLE board도 nRF52840이므로 radio 하드웨어 경로는 맞는다. tri-state와 기존 hold-tap/sticky/combo/mouse behavior는 central에서 처리되는 key position event 위에 있으므로 transport 교체 자체로 키맵을 수정할 이유가 없다.

호환성 판정은 다음과 같다.

| 조합 | 판정 | 근거/조치 |
|---|---|---|
| ZMK 0.4 + Zephyr 4.1 | 조건부 호환 | ESB 모듈의 목표 조합. 실제 세 firmware CI 빌드 필요 |
| XIAO BLE / nRF52840 | 호환 후보 | Nordic ESB 지원 SoC. 예제의 좌우는 nice_nano이므로 XIAO 좌우 빌드 검증은 별도 필요 |
| tri-state | 구조상 호환 | keymap/behavior 파일을 변경하지 않고 회귀 빌드 |
| ZMK pointing/mouse key | 구조상 호환 | central의 기존 behavior 처리 유지. queue/stack 크기 검증 필요 |
| Prospector 새 화면 | 패치 필요 | BLE observer/output 및 source index 가정 제거 필요 |
| GitHub Actions | 조건부 호환 | 패치된 NCS/nrfxlib와 모든 dependency SHA pin, 7개 matrix 빌드 검증 필요 |

## 5. 패치된 NCS와 nrfxlib가 필요한 이유

ESB 모듈 README는 ZMK 0.4/Zephyr 4.1 조합에서 다음 두 project를 요구한다.

- `badjeff/sdk-nrf`의 `v3.1-branch+zmk-fixes`
- `nrfconnect/sdk-nrfxlib`의 대응 `v3.1-branch`

이 조합은 NCS 3.1 계열 nrfx ESB library와 Zephyr 4.1 CMake/Kconfig 검증을 맞추기 위한 것이다. 일반 ZMK manifest의 Nordic project 조합만 사용하면 ESB library/CMake 구성이 일치하지 않는다. `badjeff/zmk` fork는 ESB 모듈 자체의 필수 의존성이 아니므로, upstream ZMK로 빌드가 성립하면 추가하지 않는다.

## 6. 두 peripheral을 하나의 central이 처리하는 방식

왼쪽은 wire ID 1, 오른쪽은 wire ID 2를 사용한다. 세 firmware는 하나의 공통 DTSI에 정의한 base address와 prefix를 공유하고, ID 1/2는 서로 다른 ESB data pipe에 매핑된다. Prospector central은 수신 envelope의 source와 ZMK split event를 읽어 하나의 ZMK event stream으로 병합한다.

upstream은 wire ID와 ZMK logical source index를 동일하게 사용하지만, ZMK/Prospector 배열은 0부터 시작한다. 최소 패치는 다음을 보장해야 한다.

- peripheral → central: wire ID 1/2를 ZMK logical source 0/1로 변환
- central → peripheral command: logical source 0/1을 wire ID 1/2로 변환
- available sources: 0과 1을 모두 반환
- battery 및 Prospector widget도 logical source 0/1만 사용

이렇게 해야 peripheral count를 실제 개수 2로 둘 수 있고 오른쪽 배터리의 out-of-bounds를 막을 수 있다.

## 7. ACK, 재전송, sequence 처리

upstream 기본 경로는 다음 기능을 제공한다.

- ESB hardware ACK
- 600 us retransmit delay와 built-in retransmit count 3
- key/sensor/battery event의 application retry(채널 hopping 사용 시 기본 최대 6)
- CRC postfix 및 CRC 오류 시 RF channel 변경
- link 실패 시 RF channel hopping
- key position auto-heal

신뢰성이 필요한 key press/release에는 ACK와 retry를 유지한다. benchmark 수치를 위해 ACK를 끄지 않는다.

그러나 upstream `peripheral.c`의 `evt_msg_id`는 로컬 TX ring 메타데이터로 덧붙고, `common.c`가 실제 radio 길이를 메타데이터 앞에서 잘라 central로 보내지 않는다. 또한 현재 TX message ID는 전역 값이라 여러 event가 queue될 때 retry bookkeeping이 다른 packet과 연결될 위험이 있다. 따라서 현재 upstream 상태만으로 central의 received sequence, gap 및 retransmission을 증명할 수 없다. 호환 패치에서는 wire sequence와 source tick을 payload에 포함하고, queue 항목별 ID 및 central per-source counter를 둔다. payload는 48 bytes 이하임을 `BUILD_ASSERT`로 검증한다.

## 8. Prospector 화면 task의 영향

Prospector `ed98221f...`는 adapter가 활성화되면 `src/split/bluetooth/central_status_changed_observer.c`를 무조건 컴파일한다. 이 파일은 Zephyr Bluetooth와 ZMK BLE API를 사용한다. Operator `output.c`도 BLE profile/event API를 무조건 사용하며, battery widget은 `ZMK_SPLIT_BLE_PERIPHERAL_COUNT`와 event source 직접 indexing을 가정한다. 따라서 ESB-only 빌드에는 다음 최소 패치가 필요하다.

- BLE central observer를 transport-agnostic split peer observer로 교체
- Operator output widget을 USB-only 상태로 컴파일 가능하게 함
- source 0/1과 peripheral count 2 사용
- ESB heartbeat/last-seen timeout으로 실제 left/right 상태 event 생성

화면은 release firmware에 유지한다. ESB radio IRQ와 USB HID 경로는 LVGL 화면 work보다 높은 우선순위를 가져야 한다. display update/animation이 queue latency에 미치는 영향은 코드만으로 수치화하지 않고, display-on release와 display-off benchmark 빌드를 동일 부하에서 비교한다.

## 9. 배터리 상태 전달

ESB 모듈은 BLE 이름을 가진 ZMK battery Kconfig를 `!ZMK_SPLIT_BLE`일 때도 제공하며, peripheral battery event를 serialize하고 central의 표준 `zmk_peripheral_battery_state_changed` 처리기로 전달한다. 이름만 보고 이 설정을 제거하면 안 된다.

wire 전달 자체는 지원되지만 upstream ID 1/2와 Prospector 0/1 배열 가정 때문에 그대로는 오른쪽 값이 안전하지 않다. source mapping 패치 후 화면 widget이 logical source 0/1을 받는지 빌드 및 하드웨어에서 검증한다. 실제 배터리 값과 화면 표시는 하드웨어가 없으므로 현재 **미측정**이다.

## 10. USB 1K와 fresh radio 1K의 차이

- **USB 1K polling**: Full-Speed HID interrupt endpoint의 `bInterval=1`; host가 최대 1 ms 간격으로 IN transaction을 요청할 수 있다.
- **fresh radio event delivery**: 각 하프에서 새로 만들어진 sequence가 central에 도착한 간격과 loss를 말한다.
- **end-to-end latency**: matrix scan/debounce → peripheral queue → ESB airtime/retry → central queue/behavior → USB report → host 수신까지의 시간이다.

동일 HID report를 매 USB poll에 반복하는 것은 fresh-event 1K가 아니다. 최종 release에서 1K급이라고 말하려면 peripheral별 sequence, central RX inter-arrival, 양쪽 동시 부하 loss 및 p95/p99를 실제 장비로 기록해야 한다. 목표값은 구조적 목표이며 실측값이 아니다.

## 11. 암호화·인증과 보안 한계

Nordic ESB의 address, CRC 및 ACK는 암호화나 peer 인증이 아니다. 이번 transport에는 기밀성, 송신자 인증 및 replay 방지가 없다. 같은 address를 아는 근처 장치는 packet을 관찰하거나 위조할 수 있다. 생성하는 address는 다른 기본 예제와의 우발적 충돌을 줄이는 식별자일 뿐 비밀키가 아니다. 이 제한을 README와 flash 문서에 명시한다.

## 12. 양쪽 동시 입력의 1 ms급 처리 검증

코드 검증만으로 동시 입력 성능을 확정하지 않는다. benchmark firmware와 도구는 최소 다음을 분리 기록해야 한다.

1. source 0/1의 monotonic sequence 및 수신 packet count
2. per-source sequence gap, duplicate/out-of-order
3. retransmission, CRC/checksum failure 및 queue overflow
4. dongle RX inter-arrival과 USB report enqueue 시각
5. 한쪽 단독, 양쪽 동시, display on/off, RF 혼잡 조건
6. 각 조건 100,000 event 이상에서 loss
7. typical, p95, p99 및 최대 지연

source와 dongle clock이 동기화되지 않은 상태의 timestamp 차이는 latency로 보고하지 않는다. one-way latency는 GPIO/logic analyzer 또는 왕복 clock offset 보정으로 측정하고, USBPcap/Wireshark로 USB interrupt interval을 별도로 검증한다.

## 선택한 구현 경계

- 기존 `totem_left`, `totem_right`, `totem_dongle prospector_adapter`, `settings_reset` 빌드는 유지한다.
- ESB는 auxiliary shield/config fragment로 추가하여 기존 `config/totem.keymap`을 수정하지 않는다.
- 새 address는 공통 DTSI 한 곳에만 둔다.
- release는 display enabled, benchmark는 display disabled 변형을 추가할 수 있다.
- 외부 project는 최종적으로 SHA pin하고 각 upstream license를 보존한다.
- 실제 하드웨어 계측이 끝나기 전 결과 표의 fresh-event rate, loss 및 end-to-end latency는 `미측정`으로 남긴다.

## 참고 구현

- <https://github.com/badjeff/zmk-feature-split-esb/tree/1f4cd4558bb9e0626ec2507f334f239862af859d>
- <https://github.com/badjeff/zmk-config/tree/594a7d5b962d252036c8391193fda3d3da659573/boards/shields/donki36>
- <https://github.com/carrefinho/prospector-zmk-module/tree/ed98221f3b52b7066dbb10ba3af8a29150b93a5a>
- <https://github.com/zmkfirmware/zmk/tree/904c9aec8822d79149d42c8a9a77e8828eb08f5a>
