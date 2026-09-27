# ESB 안정성 패치

기준 소스는 `feature/totem-prospector-esb-secure-v3`의 `da908cf9918655c8943583cd4284c31d017af6be`이다. 이 브랜치의 v2/v3 빌드 모두에 공통 수정이 적용된다. 기존 `feature/totem-prospector-esb-1k` 브랜치 자체는 수정하지 않는다.

## 입력 복구

- 기존 `KEY_POSITION_EVENT`의 press/release 전송을 유지한다.
- 새 wire type `ESB_WIRE_EVENT_KEY_STATE=6`은 현재 38개 위치의 5-byte bitmap이다. 기존 wire enum 값은 바꾸지 않는다.
- Peripheral은 큐가 가득 차더라도 실제 키 상태를 갱신한다. 기본 250ms heartbeat, 큐 넘침 후, v3 세션 확정 후 snapshot을 보낸다.
- Snapshot과 key edge는 같은 mutex 아래에서 같은 송신 FIFO에 들어간다. 연결 전에 보관한 이벤트를 모두 큐에 넣은 다음 snapshot을 추가한다.
- Central은 source/pipe, payload 길이, 범위 밖 bitmap bit, session 및 sequence를 확인한다. v3는 기존 active key로 bitmap을 암호화·인증한다.
- 수신 상태와 차이가 있는 위치만 반영한다. 빠진 release를 먼저 적용한 다음 현재 눌린 키를 복원한다. 같은 snapshot은 새 입력을 만들지 않는다.

무선 연결이 복구되고 snapshot이 도착하면 상태를 맞출 수 있다. 250ms는 송신 예약 주기이며 장애 중 복구 시간의 보장값이 아니다. Press와 release가 모두 유실된 짧은 tap이나 과거 combo/hold-tap의 정확한 타이밍은 현재 bitmap으로 복원할 수 없다.

### 선행 눌림이 없는 해제 방어

Central이 추적하는 해당 source·position이 이미 해제 상태이면, 새 wire release를 ZMK behavior에 전달하지 않는다. 눌림 edge가 유실된 뒤 해제만 도착하는 경우, 상대 이동 behavior는 매 해제마다 속도를 빼므로 정지 상태에서 반대 방향 이동이 시작될 수 있다. 이 orphan-up 경로는 소스에서 확인한 조건부 결함이다. 정상 press/release와 기존 중복 press 복구는 유지하며, 상태를 먼저 갱신하는 snapshot의 합성 전이와 source 단절 해제에는 wire-edge 필터를 적용하지 않는다.

사용자가 보고한 간헐적인 약 1초 키 반복·고착이 이 경로 때문에 발생했는지는 아직 확인되지 않았다. 방어 코드와 host 회귀만으로 실제 증상의 원인을 확정하거나 해결됐다고 판단하지 않는다. 마우스 곡선 조정과 별개로 장치에서 재현·해제·재연결을 확인해야 한다. RF는 기존 2Mbps/+8dBm과 hopping 비활성화를 유지한다.

## 인터럽트와 ACK 큐

`NVIC_SetPriority(RADIO_IRQn, 0)`을 제거해 Zephyr `irq_lock()`과 SDK 내부 FIFO 보호가 유효하도록 한다.

Pinned `badjeff/sdk-nrf@9b3d2623fdcd9c0fd0284f860beea924568c9826`의 `esb.c`/`esb.h`에 작은 per-pipe ACK API를 추가한다. 다른 pipe의 RX/PID 상태를 초기화하지 않고 대상 ACK만 취소하며, ACK 송신 중에는 정리를 미룬다. `CMakeLists.txt`가 ESB 빌드에 이 두 파일을 적용하고 BLE 빌드에서는 원본을 복원한다.

PRX hardware에는 pipe당 ACK 하나만 둔다. 소프트웨어 큐는 각 pipe의 순서를 보존하면서 다른 준비된 pipe를 처리한다. 바이트와 local `msg_id`가 모두 같은 frame의 재접수만 합치며, 재접수로 원래 만료 시각을 연장하지 않는다. 바이트가 같아도 새 명령이면 보존하므로 v2의 press/release/press 순서를 바꾸지 않는다. 기본 `CONFIG_TOTEM_ESB_ACK_TTL_MS=1500`을 넘긴 응답은 폐기한다. 일반 command에 별도 application delivery 보장을 추가한 것은 아니며, handshake는 기존 상태 머신이 다시 시도한다.

Session 폐기/교체 시 해당 source의 producer ring, 소프트웨어 큐, hardware ACK를 정리한다. 이미 전송된 이전 세션의 ciphertext는 v3 session/sequence 검사가 거부한다.

## 전력 관리

PTX 송신 큐와 radio가 모두 유휴일 때 `CONFIG_TOTEM_ESB_HF_IDLE_MS=10`의 유예 후 HFCLK 요청을 해제한다. 다음 송신은 비동기 clock 요청 완료 후 시작한다. IRQ에서 클록 시작을 기다리지 않으며 ESB를 재초기화하지 않아 PID와 재전송 상태를 유지한다. PRX 동글은 비동기 수신을 위해 클록을 계속 유지한다.

v3 연결 탐색은 초기 2ms에서 최대 `CONFIG_TOTEM_ESB_V3_HANDSHAKE_MAX_INTERVAL_MS=1000`까지 늘어난다. 키 입력 및 무선 ACK 성공 시 빠른 탐색으로 복귀한다. 무선 ACK는 암호화 세션 인증을 대신하지 않으며 ZMK 입력은 기존 authenticated handshake 완료 후에만 수신한다.

## 검증

기존 Python protocol model 테스트에 더해 `tests/test_esb_firmware.py`가 firmware에서 사용하는 실제 C helper를 컴파일하고 실행한다. 상태 재동기화, 누락 release, 재연결 held key, 반복 snapshot, 좌우 상태 분리, bitmap 경계, 탐색 backoff, PRX 큐의 순서·격리·만료를 검사한다. CI는 C 컴파일러가 없는 경우 실패하도록 설정한다.

```sh
CC=cc python3 -m unittest -v tests/test_esb_firmware.py tests/test_esb_clock_runtime.py tests/test_esb_ack_runtime.py
python3 -m unittest -v tests/test_esb_v3_protocol.py
```

클록 runtime 테스트는 실제 firmware의 clock callback 두 개를 추출해 비동기 완료·유휴 해제·송신과 해제의 경합·실패 후 재시도를 검사한다. ACK runtime 테스트는 실제 SDK 함수 두 개를 추출해 pipe별 취소 및 진행 중 ACK 보호를 검사한다. 두 테스트의 드라이버와 IRQ는 deterministic fake이다.

`tests/test_esb_key_dispatch_runtime.py`는 실제 central의 wire dispatch·snapshot·source 해제 helper를 추출해 orphan release와 정상 전이, 상태 복구를 검사한다. ZMK 경계는 fake이며, orphan-up 방어를 제거한 대조 변형이 해당 시나리오에서 실패하는지도 확인한다. 실제 키 반복의 원인을 재현한 하드웨어 시험은 아니다.

Host C 테스트는 nRF52840 RADIO, Zephyr scheduler, PSA backend를 실행하지 않는다. 전체 firmware compile/link는 GitHub Actions의 BLE/v2/v3 16-entry matrix로 확인한다. 실물 검증에서는 다음을 확인한다.

1. 키를 누른 채 동글만 재부팅한 뒤 눌림 상태와 해제가 복구되는지 확인한다.
2. 동글이 없는 동안 입력 큐를 채운 뒤 연결해, 손을 뗀 키가 남지 않는지 확인한다.
3. 한쪽을 끈 상태에서 다른 쪽의 입력·재연결·downlink가 계속 동작하는지 확인한다.
4. 마지막 release 전송 구간의 간섭 후 정상 수신이 돌아오면 snapshot으로 해제되는지 확인한다.
5. 유휴 후 첫 입력 지연, 양쪽 동시 입력 loss/p95/p99, 동글 부재 시 전류를 기존 빌드와 비교한다.

Benchmark의 wire 6은 key_state로 표시한다. Snapshot으로 복원한 입력에는 개별 원본 edge가 없으므로 USB 상관 로그에 `BENCH_USB_UNMATCHED`가 나타날 수 있다. 장애 복구 실험은 정상 단일키 입력 latency 측정과 구분한다.

## 업데이트

Snapshot 기능을 사용하려면 왼쪽·오른쪽·동글을 모두 이 패치의 같은 v2 또는 v3 프로필로 빌드해 업데이트한다. 이전 receiver는 새 snapshot type을 처리하지 못하므로 혼합 버전의 복구 동작은 지원하지 않는다.

기존 production v3 key set을 유지한다. 공개 Actions의 `ci_only_*_testkey` UF2는 공개 테스트키를 사용하므로 production firmware로 사용하지 않는다. Production 키를 저장소나 CI에 추가할 필요는 없다. 키 파일과 로컬 빌드 절차는 [v3 빌드 문서](esb-v3-build-flash.md)를 따른다.
