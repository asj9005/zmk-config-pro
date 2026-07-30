# Totem + Prospector ZMK firmware

Seeed XIAO BLE/nRF52840 기반 Totem 좌우 하프와 Prospector USB 동글용 ZMK 설정이다. 기존 BLE split 빌드를 롤백용으로 보존하고, Nordic ESB 2.4 GHz split transport를 사용하는 저지연 프로필을 별도로 제공한다. 기존 keymap, tri-state, hold-tap, sticky key, combo, mouse/pointing 동작은 공통 `config/totem.keymap`을 사용한다.

## 빌드 구분

| 프로필 | 왼쪽 | 오른쪽 | Prospector 동글 |
| --- | --- | --- | --- |
| 기존 BLE split | `totem_left` | `totem_right` | `totem_dongle prospector_adapter` |
| ESB v2 release(평문) | `totem_left totem_esb_left` | `totem_right totem_esb_right` | `totem_dongle prospector_adapter totem_esb_dongle` |
| ESB v2 benchmark | v2 release 조합 + `totem_esb_benchmark` | v2 release 조합 + `totem_esb_benchmark` | v2 release 조합 + `totem_esb_benchmark` |
| ESB Secure v3 후보 | v2 release 조합 + `totem_esb_v3` | v2 release 조합 + `totem_esb_v3` | v2 release 조합 + `totem_esb_v3` |
| ESB Secure v3 benchmark | v3 조합 + `totem_esb_benchmark` | v3 조합 + `totem_esb_benchmark` | v3 조합 + `totem_esb_benchmark` |

`build.yaml`에는 기존 BLE 3개, `settings_reset`, ESB v2 release 3개, ESB v2 benchmark 3개와 v3 release/benchmark 6개 등 총 16개 항목이 있다. v2 기준선은 [GitHub Actions run 30433985080](https://github.com/asj9005/zmk-config-pro/actions/runs/30433985080)에서 10/10 빌드됐다. Artifact를 내려받아 ZIP을 푼 뒤 다음 v2 release 파일을 사용한다.

- `totem_left_esb.uf2`
- `totem_right_esb.uf2`
- `totem_dongle_esb_prospector.uf2`

Benchmark artifact는 각각 `_benchmark.uf2`로 끝나며 일상 사용용이 아니다. 공개 Actions의 v3 artifact에는 저장소에 공개된 폐기용 test key가 들어 있으므로 보안용 또는 일상용으로 플래시하지 않는다. 실제 v3 firmware는 로컬에서 생성한 production key로 세 역할을 빌드해야 한다. ZIP 자체는 플래시하지 않는다. 자세한 절차는 [ESB v2 빌드 및 플래시](docs/esb-1k-build-flash.md)와 [Secure v3 빌드 및 플래시](docs/esb-v3-build-flash.md)를 따른다.

## “1K”의 의미

ESB 동글은 `CONFIG_USB_HID_POLL_INTERVAL_MS=1`로 USB Full-Speed HID interrupt endpoint의 1 ms polling interval을 요청한다. 이것만으로 좌우 하프의 새 입력이 1,000 Hz로 도착한다고 주장하지 않는다.

Fresh-event 검증은 부팅 때 생성되는 32-bit session ID와 source별 sequence, gap, 수신 간격, retransmission 및 queue overflow를 함께 측정한다. USB에서 같은 HID report를 반복한 결과는 fresh radio event rate가 아니다.

현재 상태:

- ESB v2 전송 및 benchmark 코드 경로: 구현됨, 기준선 10개 CI 빌드 **성공**
- Secure v3 코드 경로: 구현됨, 16-entry 전체 matrix의 compile/link 검증은 별도 기록
- release debounce: press 1 ms, release 5 ms
- Prospector 화면, peer 상태 및 battery event 코드 경로: 포함됨, 실기 **미측정**
- v2 기준선의 컴파일된 release/benchmark 동글 HID descriptor `bInterval=1`: **확인**
- v3 동글 HID descriptor 및 실제 USB cadence: **미측정**
- 실제 USB enumerate 및 USBPcap 1 ms cadence: **미측정**
- 실제 ESB typical/p95/p99 latency: **미측정**
- 양쪽 동시 입력과 source별 100,000-event loss: **미측정**
- 실제 Prospector 화면 및 좌우 battery 표시: **미측정**

컴파일 성공 또는 descriptor 설정 확인은 실기 1K 달성과 별개다. 측정되지 않은 결과를 실측값처럼 사용하지 않는다.

## ESB 구조와 제한

`CONFIG_ESB_PIPE_COUNT=3`이며 pipe 0은 예약되어 있다. Pipe 1은 왼쪽, pipe 2는 오른쪽의 uplink와 해당 하프로 돌아가는 PRX ACK payload를 함께 담당한다. 동글은 독립적인 downlink를 선제 송신하지 못하며, command는 대상 half의 다음 uplink/heartbeat에 대한 ACK payload로 전달된다. Command application retry는 현재 0이다.

Nordic PRX의 hardware ACK-payload FIFO는 pipe별로 따로 비우는 API가 없다. 이미 FIFO에 들어간 command의 대상 half가 전송 전에 꺼지면 그 entry가 남아 다른 half의 reverse command를 지연시킬 수 있다. Half→dongle key uplink 자체에는 해당하지 않는 제한이지만 실제 장치에서 확인해야 한다.

왼쪽과 오른쪽은 별도 pipe를 쓰지만 하나의 RF channel과 하나의 동글 radio를 공유한다. TDMA나 CSMA는 없다. 고정 retry 충돌을 줄이기 위해 hardware retry delay를 왼쪽 500 µs, 오른쪽 800 µs로 다르게 두었지만, 양쪽 동시 fresh 1K와 무손실을 보장하지 않는다. RF channel hopping도 PTX/PRX 동기화가 없어 비활성화되어 있다.

각 half는 부팅마다 하드웨어 entropy로 non-zero random session ID를 만든다. Dongle은 session 변경이나 peer timeout 때 이전에 눌린 것으로 남은 key를 release하고 sequence 상태를 재설정한다. 이는 재부팅 복구 및 stuck key 완화용이며 보안 nonce나 인증 수단은 아니다.

## 주소와 보안

세 ESB 역할은 `config/boards/shields/totem/totem_esb_addr.dtsi`의 주소 정의 하나를 공유한다. 주소를 바꿀 때는 세 개 prefix와 pipe 0/1/2 매핑을 유지하고 왼쪽, 오른쪽, 동글을 모두 다시 빌드해 함께 플래시한다.

ESB 주소, CRC, ACK 및 v2 session ID는 암호화나 인증이 아니다. 평문 ESB v2에는 payload 암호화, 송신자 인증 및 replay 방지가 없다. 주소는 주변의 다른 ESB 세트와 우발적으로 충돌할 가능성을 낮추는 식별자일 뿐 비밀키가 아니다.

Secure v3 후보는 half별 128-bit PSK, AES-128-CCM/MIC4, 두 random nonce로 파생한 session key와 엄격한 sequence 검사를 추가한다. Active/pending traffic에서 MIC 인증이 실패하면 BLE처럼 기존 논리 link와 traffic key를 폐기하고 새 handshake를 요구한다. 이 처리는 error path에만 있어 정상 packet hot path의 암호 연산 수는 늘리지 않는다. Hardware 실측에서 v2 대비 반응성 승인 기준을 통과하기 전까지 일상용 1K firmware로 승인하지 않는다. RF jamming, 인증 실패를 이용한 재연결 DoS, traffic timing 분석, production UF2·SWD에서의 key 추출, relay 공격과 forward secrecy는 해결하지 않는다.

## 사용상 주의

- ESB와 기존 BLE 역할 firmware를 한 세트 안에서 섞으면 통신하지 않는다.
- 평문 ESB v2와 Secure v3 역할 firmware도 서로 통신하지 않는다. 세 장치를 같은 세대와 같은 key set으로 함께 플래시한다.
- 최초 BLE↔ESB 전환 시 세 장치에 `settings_reset`을 적용한 뒤 역할별 firmware를 플래시한다.
- PC에는 Prospector 동글을 연결한다. 왼쪽과 오른쪽 ESB peripheral은 USB host 출력용이 아니다.
- ESB의 +8 dBm 출력, ACK/retry 및 저지연 설정은 기존 BLE보다 배터리 소비를 늘릴 수 있다. 실제 사용 시간은 **미측정**이다.
- Release와 현재 benchmark 동글은 모두 Prospector 화면을 유지한다. Display-disabled artifact는 아직 구현되지 않았다.
- Benchmark는 0 ms debounce와 per-packet RTT logging을 사용하므로 release 성능과 동일하게 해석하지 않는다.

## 문서

- [ESB 1K 타당성 검토](docs/esb-1k-feasibility.md)
- [ESB 빌드 및 플래시](docs/esb-1k-build-flash.md)
- [ESB 1K benchmark](docs/esb-1k-benchmark.md)
- [ESB Secure v3 보안 설계와 검증 상태](docs/esb-v3-security.md)
- [ESB Secure v3 production 빌드 및 플래시](docs/esb-v3-build-flash.md)
