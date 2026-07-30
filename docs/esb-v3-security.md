# Totem + Prospector ESB Secure v3 설계와 검증 상태

작성 기준일: 2026-07-30

작업 브랜치: `feature/totem-prospector-esb-secure-v3`

## 문서 상태

이 문서는 구현 전에 고정한 보안 경계와 성능 승인 기준, 그리고 구현 후 검증 상태를 함께 기록한다. AES-128-CCM/MIC4, 두 nonce handshake, session key, replay 방지, production key provisioning과 benchmark 계측 경로는 firmware에 구현됐다. CI compile/link 성공과 실제 장치의 1K 성능 승인은 별도 단계이며, 실측하지 않은 값을 구현 결과로 바꾸어 쓰지 않는다.

우선순위는 다음과 같다.

1. 기존 ESB v2의 1K급 반응성을 최우선으로 보존한다.
2. 그 범위 안에서 기존 ZMK BLE split과 비슷한 수준의 기밀성, 무결성, 상대 인증 및 replay 방지를 제공한다.
3. 보안 적용 후 반응성이 승인 기준을 만족하지 않으면 v3를 실사용 firmware로 채택하지 않는다. 이 경우 기존 평문 ESB v2를 그대로 유지하고 실패 원인과 측정 결과만 보고한다.
4. BLE 수준을 넘는 강화안은 성능을 해치지 않을 가능성이 있어도 이번 v3에서 구현하지 않고 검토 항목으로만 남긴다.

따라서 v3의 build 성공이나 USB descriptor의 `bInterval=1`만으로 “보안을 유지한 1K”라고 주장하지 않는다.

## 1. 목표와 비범위

v3의 보호 대상은 왼쪽/오른쪽 Totem half와 Prospector dongle 사이의 ESB 무선 payload다. 다음 속성을 목표로 한다.

- key position, press/release, battery 및 split command payload의 기밀성
- PSK를 가진 정당한 half와 dongle이 보낸 packet인지 확인하는 송신자 인증
- 전송 중 packet 변조 검출
- 같은 session 안의 중복, 순서 역전 및 과거 packet replay 차단
- half 또는 dongle 재부팅 뒤 이전 session packet을 다시 주입하는 cross-session replay 차단
- 왼쪽 key 유출이 오른쪽 link의 위조로 바로 이어지지 않는 key 분리
- 기존 ESB ACK/retry, Prospector 화면, battery, keymap 및 USB 1 ms polling 유지

다음 항목은 이번 v3의 비범위다.

- RF jamming이나 의도적인 충돌에 대한 가용성 보장
- packet 길이와 송신 시각까지 숨기는 traffic-flow confidentiality
- relay 공격을 이용한 물리적 거리 증명 우회 방지
- 동글 없는 Bluetooth host 연결
- 사용자 입력을 받는 over-the-air pairing UI
- Secure Boot, 서명된 firmware update 또는 debug port 영구 잠금
- 장치 탈취 후 flash/RAM을 읽는 공격자에 대한 완전한 key 보호

ESB 주소, Nordic radio CRC, ACK와 재전송은 보안 수단이 아니다. 이 기능들은 주소 충돌 방지와 전송 신뢰성을 담당하며 v3의 암호화·인증을 대신하지 않는다.

## 2. “기존 BLE 수준”의 의미

기존 ZMK BLE split은 BLE link-layer의 AES-CCM 암호화와 128-bit key를 사용한다. 일반적인 Just Works pairing은 암호화와 무결성을 제공하지만 사용자가 숫자를 비교하거나 별도 인증 정보를 입력하지 않으므로 MITM에 대해 인증된 pairing으로 간주할 수 없다. BLE 암호화 packet의 MIC는 4 byte다. Bluetooth LE Link Layer는 암호화된 data PDU의 인증 실패를 감지하면 해당 connection을 lost로 간주한다. v3도 active/pending traffic의 MIC 실패 한 번에 논리 link와 traffic key를 폐기하고 새 handshake를 요구한다. [Bluetooth Core Specification — Low Energy Link Layer Security](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/low-energy-controller/low-energy-link-layer-security.html)

v3는 BLE protocol을 ESB 위에 그대로 이식하지 않는다. 다음 보안 속성을 BLE 수준의 기준으로 삼는다.

| 속성 | 기존 BLE 기준 | ESB Secure v3 목표 |
|---|---|---|
| 암호 알고리즘 | AES-CCM | AES-128-CCM |
| 장기 key 크기 | 128 bit | half별 128-bit PSK |
| packet 인증 tag | 4-byte MIC | 4-byte MIC |
| 기밀성 | link payload 암호화 | ESB application body 암호화 |
| 상대 인증 | bond key 보유 확인 | 사전 주입한 PSK 보유 확인 |
| replay 방지 | connection counter/nonce | 양측 random handshake, session 및 sequence |
| over-the-air pairing | BLE pairing/bonding | 없음; local provisioning만 사용 |
| forward secrecy | 제공하지 않음 | 제공하지 않음 |

“BLE 수준”은 위 속성의 목표가 비슷하다는 뜻이지, 두 구현의 공격 표면과 key lifecycle이 동일하다는 뜻은 아니다. v3의 PSK는 firmware에 사전 주입되며 BLE bonding처럼 현장에서 새 peer를 추가하는 기능이 없다. 반대로 안전한 local provisioning으로 생성한 충분히 random한 PSK를 사용하면, 무선 pairing을 가로채는 공격 표면은 생기지 않는다.

## 3. 위협 모델

v3는 다음 능력을 가진 근거리 무선 공격자를 가정한다.

- ESB packet을 수동 수집
- 수집한 packet의 재전송
- 임의 packet 생성 및 주입
- packet의 일부 bit 또는 header 변조
- 왼쪽 또는 오른쪽 source를 사칭
- half나 dongle을 재부팅시킨 뒤 과거 capture를 다시 사용

PSK를 알지 못하는 공격자는 암호화된 key body를 읽거나 유효한 MIC를 새로 만들 수 없어야 한다. 인증에 실패한 packet은 ZMK event나 battery/display 상태로 승인되지 않는다. Active 또는 pending traffic key로 검증하다 MIC가 실패하면 해당 논리 link의 눌린 key 상태, active/pending traffic key와 sequence 상태를 폐기하고 새 handshake 전까지 data를 수락하지 않는다.

다음 공격자는 보호 범위 밖이다.

- RF를 계속 점유하여 정상 packet을 막는 jammer
- production UF2, SWD, flash dump 또는 동작 중 RAM을 읽어 PSK를 얻은 공격자
- 정상 장치 사이 packet을 그대로 실시간 relay하는 공격자
- PC, ZMK keymap 또는 USB host 자체를 장악한 공격자

## 4. 암호 profile

### 4.1 기본 primitive

v3의 기본 profile은 다음으로 고정한다.

- cipher: AES-128-CCM
- authentication tag: 4 byte(32-bit MIC)
- 장기 key: 왼쪽과 오른쪽에 각각 독립적인 128-bit PSK
- session key derivation: AES-CMAC
- implementation API: NCS 3.1.1 PSA Crypto
- nRF52840 기본 backend: heap과 전역 driver lock이 없는 Oberon PSA
- 검토 전용 대안: CryptoCell 310/CC3XX A/B benchmark

4-byte MIC는 기존 BLE link-layer와 같은 tag 길이를 선택하여 packet당 airtime과 암호 처리량 증가를 최소화한다. 임의 위조 한 번의 성공 확률 상한은 약 `2^-32`이며 시도 횟수가 늘면 누적 위험도 증가한다. 따라서 MIC4는 무제한 공격에 대해 충분한 장기 보안 강도를 제공한다는 뜻이 아니라, 사용자가 요구한 BLE 수준과 1K 우선 조건의 절충안이다.

왼쪽 PSK는 왼쪽 firmware와 dongle에만, 오른쪽 PSK는 오른쪽 firmware와 dongle에만 들어간다. 한쪽 half firmware의 key가 유출되어도 다른 half의 packet을 복호화하거나 위조할 수 없어야 한다.

### 4.2 보호 범위와 wire format

v3 packet은 보안 profile을 평문 v2와 혼동하지 않도록 별도 protocol version 또는 magic을 사용한다. v2와 v3 firmware를 섞은 경우 인증되지 않은 fallback을 시도하지 않고 통신에 실패해야 한다. 평문으로 자동 downgrade하는 경로는 두지 않는다.

정상 data packet의 예정 보호 범위는 다음과 같다.

- cleartext이지만 AAD로 인증: protocol version/magic, payload size, source, direction, message kind, 64-bit session 식별자, 32-bit sequence, source tick
- 암호화 및 인증: key position, pressed/released, battery 값, sensor/input data, command body
- packet postfix: 4-byte CCM MIC

Header를 AAD로 포함하므로 source, direction, type, sequence 또는 길이를 바꾸면 인증이 실패한다. Body만 암호화하므로 radio 수신 뒤 올바른 PSK와 nonce를 선택하는 데 필요한 최소 header는 cleartext로 남는다. 수신 측은 cleartext source가 실제 ESB RX pipe와 일치하는지 CCM key lookup 전에 확인하므로 다른 pipe를 tag 검증 경로로 사용할 수 없다.

Nonce는 한 session 안에서 절대 재사용하지 않도록 source/direction domain, 64-bit session 식별자와 32-bit sequence를 조합한 13-byte 값을 사용한다. Uplink와 downlink는 direction domain이 다르므로 같은 sequence 값도 nonce가 충돌하지 않는다. Sequence가 wrap하기 전에 반드시 새 handshake를 수행하고 새 session key로 전환한다.

v3에서는 application CRC32를 CCM MIC로 대체한다. Nordic radio CRC16은 전송 오류를 빠르게 걸러내기 위해 유지하고, ESB ACK/retry도 기존 신뢰성 동작을 위해 유지한다.

암호화 뒤에도 source, packet 길이와 송신 시각은 관찰될 수 있다. 모든 packet을 고정 길이로 padding하거나 일정 주기로 dummy packet을 보내면 traffic pattern은 더 숨길 수 있지만 airtime과 배터리를 증가시키므로 v3에는 포함하지 않는다.

## 5. 양측 random handshake와 session

### 5.1 개요

장기 PSK를 모든 data packet의 고정 traffic key로 직접 반복 사용하지 않는다. Half와 dongle이 각각 새 random 값을 제공하고, 인증된 handshake transcript와 PSK에서 128-bit session key를 도출한다.

예정 흐름은 다음과 같다.

1. Half가 hardware CSPRNG로 64-bit 이상의 새 `half_nonce`를 생성한다.
2. Half가 자신의 source와 `half_nonce`를 PSK로 인증한 `HELLO`를 보낸다.
3. Dongle은 MIC를 먼저 확인하고, 유효한 `HELLO`에 대해서만 새 `dongle_nonce`를 생성한다.
4. Dongle은 source, `half_nonce`, `dongle_nonce`를 묶어 인증한 `CHALLENGE`를 해당 pipe의 ACK payload로 보낸다.
5. Half는 자신이 현재 기다리는 `half_nonce`와 일치하고 MIC가 유효한 challenge만 받아들인다.
6. 양쪽은 PSK와 두 nonce에서 같은 128-bit session key와 64-bit session 식별자를 도출한다.
7. Half가 pending session key로 인증된 `READY`를 보내면 dongle이 해당 pending session을 active로 승격한다.
8. Dongle이 active session key로 인증된 `SESSION_OK`를 돌려보내고 half가 이를 검증하면 half도 session을 active로 승격한다. 그 전에는 일반 data packet을 보내거나 수락하지 않는다.

Session key는 다음 AES-CMAC 식으로 도출한다.

```text
session_key = CMAC(
    link_PSK,
    "ZmK3sess" || peripheral_nonce64_LE || dongle_nonce64_LE
)
```

`"ZmK3sess"`는 v3 session key용 8-byte ASCII domain label이며 두 64-bit nonce는 little-endian으로 직렬화한다. AES-CMAC의 128-bit 출력 전체를 AES-128-CCM session key로 사용한다. 이 구조는 BLE가 장기 key에서 connection별 session key를 만드는 방식과 보안 목적이 비슷하며, 단순 XOR 또는 자체 설계 hash 조합을 사용하지 않는다. CMAC은 handshake 때만 실행되므로 steady-state 1K packet 경로의 매 packet 비용에는 포함되지 않는다. 식과 byte order는 known-answer test로 고정한다.

하나의 link에는 하나의 session key를 사용하고 uplink/downlink는 nonce의 direction domain으로 분리한다. 방향별 별도 session key 파생은 보안 강화 후보이며 이번 BLE 수준 profile에는 포함하지 않는다.

### 5.2 Replay와 session 교체

Dongle은 source별로 active session, pending handshake, 마지막으로 승인한 uplink sequence를 독립 관리한다. Half도 downlink sequence를 별도로 검증한다.

- 같은 session/sequence packet은 중복으로 폐기한다.
- `sequence <= last_sequence`인 중복·과거 packet은 폐기한다. 현재 구현은 느슨한 replay window를 두지 않는다.
- Root-key `RECOVERY` sequence는 traffic session이 아니라 half의 현재 boot nonce에 묶인 별도 high-water로 관리한다. 이 값은 같은 boot 안의 traffic rekey와 peer timeout을 지나도 감소하지 않으며 `READY`가 과거 request 값으로 되돌리지 않는다. 이미 폐기한 traffic session을 가리키는 늦은 `RECOVERY`는 fresh root sequence만 소비한 뒤 새 pending session을 만들지 않고 폐기한다.
- 한 번 ACK 대기열에 들어간 recovery challenge의 central nonce, request, reset session과 pending key는 `READY`까지 고정한다. 다음 `RECOVERY`가 먼저 도착해도 root high-water만 전진시키고 같은 challenge를 다시 넣는다. Half는 자신이 실제 보낸 범위 안에서 아직 소비하지 않은 request를 받아들이므로 ACK payload가 한 packet 늦게 도착해도 양쪽 pending key가 갈라지지 않는다.
- Dongle은 인증된 duplicate `READY`도 peer liveness로 인정한다. Half가 `WAIT_SESSION_OK`에서 2초 동안 인증된 `SESSION_OK`를 받지 못하면 중앙이 pending key를 이미 폐기한 장기 RF 단절로 보고 새 boot nonce와 `HELLO`부터 다시 시작한다.
- MIC-failure reset pending은 2초의 bounded window 동안 다른 boot nonce의 `HELLO`가 교체하지 못한다. Dongle의 모든 미확정 pending도 2초 안에 `READY`로 확인되지 않으면 폐기한다. 따라서 동글 재부팅 직후 먼저 도착한 과거 `HELLO`/`RECOVERY` 한 장이 잘못된 root epoch를 영구 점유하지 못하며, 현재 half의 반복 신호가 다음 후보가 된다.
- Active/pending traffic의 MIC 실패는 기존 session과 pending handshake를 모두 폐기하고 fresh handshake가 끝날 때까지 fail closed한다. 이 실패 처리는 정상 packet hot path가 아니라 `-EACCES` error path에서만 실행된다.
- Dongle은 teardown 직후 폐기한 session ID에 묶인 root-authenticated reset challenge를 해당 half의 ACK 대기열에 즉시 넣는다. Half는 그 session이 현재 active/pending이거나 아직 challenge를 기다리는 동일 boot nonce일 때만 reset을 받아들이며, 이후 event는 pre-session queue에 보존한다. Challenge queue가 막힌 경우 기존 root recovery heartbeat가 fallback이다.
- Root-key `HELLO`/`RECOVERY`/`CHALLENGE` 인증 실패는 packet만 폐기한다. 이들은 encrypted data connection의 packet이 아니며, 임의 root probe 하나가 정상 active link를 즉시 끊게 하지 않는다.
- 과거 `HELLO` replay만으로 현재 active session을 끊거나 교체하지 않는다.
- Dongle은 새 session key로 유효한 `READY`를 받은 뒤에만 pending session을 active로 승격하고, half는 유효한 `SESSION_OK`를 받은 뒤에만 승격한다.
- Half와 dongle 중 어느 한쪽이 재부팅되어도 양측이 제공한 새 random 값 때문에 과거 data, `READY`와 `SESSION_OK`는 새 session에서 유효하지 않아야 한다.

동일 nonce로 CCM을 다시 수행할 때 plaintext 또는 AAD가 달라지면 안 된다. 따라서 재전송하는 `HELLO`와 `CHALLENGE`는 같은 handshake 시도 동안 최초에 만든 wire image를 그대로 재사용하거나, 완전히 동일한 authenticated content만 재생성한다.

Session 전환 시 이전 session key와 pending key는 가능한 즉시 zeroize한다. 32-bit sequence wrap이 가까워지거나 nonce uniqueness를 보장할 수 없는 오류가 발생하면 data 송신을 계속하지 않고 새 handshake로 전환한다.

### 5.3 재부팅과 입력 복구

Half 재부팅 시 새 handshake가 끝나기 전의 key event를 평문으로 보내지 않는다. Dongle 재부팅 시에도 이전 session을 추측해 받아들이지 않고 source별 새 handshake를 요구한다.

Half에는 handshake 중 새로 생성된 event를 보존하는 작은 bounded pre-session FIFO가 구현되어 있다. 다만 dongle이 MIC 실패를 감지한 직후부터 half가 reset challenge를 수신하기 전까지 구 session으로 이미 전송한 경계 event는 ESB hardware ACK를 받았더라도 application에서 승인되지 않으며 FIFO로 되돌릴 수 없다. 따라서 인증 실패·공격 경계에서 “입력 무손실”을 주장하지 않는다. 이 구간까지 무손실로 만들려면 application delivery ACK와 event journal 또는 명시적 key-state resync가 필요하며, 정상 hot path의 packet·RAM·latency 비용을 실측하기 전에는 도입하지 않는다.

## 6. Key provisioning과 artifact 정책

Production PSK는 source code, Git history, build log, public GitHub Actions artifact에 넣지 않는다.

구현된 provisioning 구조는 다음과 같다.

- Local generator가 cryptographically secure random 16-byte PSK를 왼쪽과 오른쪽에 각각 독립적으로 생성한다.
- Production build는 gitignore된 local key configuration file을 명시적으로 전달해야만 성립한다.
- Build system은 파일 형식과 길이를 검증하고 build directory 안에만 generated header를 만든다.
- 왼쪽 image에는 왼쪽 PSK만, 오른쪽 image에는 오른쪽 PSK만 포함한다.
- Dongle image에는 두 PSK가 모두 포함된다.
- 실제 key 값은 command line, compiler diagnostic 또는 script stdout에 출력하지 않는다.

공개 GitHub Actions에서는 production key를 사용하지 않는다. CI build 재현성 및 compile test를 위해 고정된 **폐기용 CI test key**만 허용하며, 생성되는 firmware와 artifact 이름에 `ci_only_..._testkey`를 명시한다. 이 image는 누구나 key를 알 수 있으므로 실제 키보드에 일상용으로 flash해서는 안 된다.

Production UF2 자체에는 장치가 사용할 PSK가 포함되므로 민감한 파일로 취급한다. Public repository가 Actions secret으로 production build를 수행하더라도 결과 UF2를 public artifact로 올리면 key 보호가 되지 않는다. Production image는 local에서 만들거나 접근이 제한된 private artifact 저장소만 사용한다.

Key를 교체할 때는 해당 half와 dongle을 함께 다시 빌드하고 flash한다. Dongle에는 두 key가 있으므로 dongle UF2가 유출되면 좌우 key를 모두 폐기하고 세 image를 모두 새 key로 재생성해야 한다.

## 7. 1K 성능 예산과 승인 gate

### 7.1 원칙

v3는 v2를 대체하는 단일 profile이 아니라 별도 후보 profile로 추가한다. 기존 BLE build, 평문 ESB v2 release/benchmark 및 settings reset build는 삭제하거나 변경하지 않는다. 사용자가 실기 비교를 끝내기 전까지 일상 사용의 기준 firmware는 검증된 ESB v2다.

성능 비교는 다음 조건을 고정한다.

- 동일한 XIAO nRF52840 세 장치와 동일한 RF 위치
- 동일한 USB port와 host
- 동일한 2 Mbps ESB, ACK/retry 및 debounce 설정
- 동일한 Prospector display 상태
- 동일한 synthetic event rate와 시험 시간
- 비교 대상 commit, firmware, clock source와 측정 시작/종료 지점 기록

20~40 byte hot path에서는 CC3XX driver lock과 고정 호출 비용이 지배적일 수 있으므로 v3 기본값은 heapless Oberon PSA다. CC3XX는 더 강한 보안을 제공하는 기능이 아니라 같은 primitive의 대체 backend이며, release 기본값으로 가정하지 않고 별도 A/B 후보로만 남긴다.

### 7.2 필수 승인 조건

다음 조건을 모두 통과하기 전에는 v3를 “1K 유지” 또는 release 승인으로 표시하지 않는다.

1. Left, right와 dongle v3 image가 빌드되고 기존 BLE/v2 전체 matrix도 계속 성공한다.
2. Dongle USB Full-Speed HID interrupt-IN descriptor가 `bInterval=1`을 유지하고 실제 USBPcap 측정에서도 1 ms service cadence에 구조적 회귀가 없다.
3. Source별 fresh sequence로 left-only, right-only 및 양쪽 동시 부하를 구분해 측정한다.
4. 양쪽 동시 1 kHz synthetic 시험에서 source별 100,000개 이상의 event에 missing sequence, accepted replay, queue overflow 및 terminal TX failure가 없어야 한다.
5. 인증 실패, replay drop, session 재협상 및 pre-session queue 통계를 source별로 기록할 수 있어야 한다.
6. CCM seal/open 처리 시간의 typical/p95/p99와 dongle에서 양쪽을 합친 crypto CPU budget을 기록한다.
7. v2와 v3의 radio fresh-event cadence 및 key detection-to-host latency를 같은 방법으로 비교한다.
8. Prospector display on 상태에서 시험하며 화면, 좌우 peer 상태와 battery 전달도 유지한다.
9. 재부팅, 잘못된 key, MIC 변조, sequence replay, cross-source packet 및 과거-session replay 시험을 모두 통과한다.
10. PSA CCM/CMAC 의존성을 포함한 세 v3 image가 flash와 RAM 한도 안에서 link되어야 한다. 특히 RAM 여유가 작은 benchmark dongle의 build 또는 runtime queue 여유가 성립하지 않으면 v3 승인을 중단한다.

1K 우선의 사전 성능 budget은 다음과 같이 둔다.

- USB polling interval: 1 ms 유지
- CCM seal 또는 open 1회의 처리 시간: p99 `100 µs` 이하 목표
- Dongle의 양쪽 합산 crypto 처리: 1 ms window의 `25%` 이하 목표
- Secure v3의 end-to-end p95/p99 증가: 동일 v2 대비 `0.25 ms` 이하 목표
- Source별 packet loss: `0 / 100,000` 목표

이 수치는 **승인용 목표값**이며 현재 실측값이 아니다. 측정 분산이나 계측 방법 때문에 위 수치가 반응성 유지 여부를 잘 나타내지 못하면 원자료와 측정 한계를 함께 공개하고 더 엄격한 판단을 택한다. 특히 평균값이 좋아도 p95/p99, 양쪽 동시 부하 또는 queue loss가 나빠지면 통과로 간주하지 않는다.

어느 필수 조건이든 실패하면 다음 원칙을 따른다.

- v3 production flash 절차를 권장하지 않는다.
- MIC를 끄거나 ACK/retry를 줄여 수치만 맞추지 않는다.
- 평문 fallback을 추가하지 않는다.
- 기존 ESB v2를 일상용으로 유지한다.
- Build/RAM 한도 실패도 성능 gate 실패와 동일하게 취급해 v3 승인을 중단한다.
- 실패한 firmware와 측정 조건, v2 대비 delta 및 병목 위치를 검토 결과로 남긴다.

## 8. 구현 및 시험 계획

구현은 기존 ESB compatibility overlay 안에 작은 v3 crypto 계층과 별도 config profile을 추가하는 방식이다. ZMK 전체 또는 ESB module 전체를 새 fork로 복사하지 않았다.

구현 단계에서 최소한 다음 시험을 추가한다.

- AES-CCM known-answer test
- 올바른 key의 encrypt/decrypt round trip
- ciphertext, AAD, nonce와 MIC 각각의 1-bit 변조 거부
- 왼쪽 packet을 오른쪽 key 또는 source로 해석했을 때 거부
- 동일 session/sequence replay 거부
- 이전 session packet과 `READY`/`SESSION_OK` replay 거부
- 재전송용 동일 wire image의 결정성 확인
- sequence wrap 전 강제 re-handshake
- dongle/half 각각의 재부팅 복구
- production key가 없을 때 local production build 실패
- CI test key artifact의 명확한 표식 확인

Release build에서는 per-packet logging을 끈다. 현재 benchmark build는 `BENCH_RX/TX/USB`를 packet별로 RTT에 출력하므로 logging overhead와 RTT drop이 timing 및 관측값에 영향을 줄 수 있다. 따라서 원본 packet log와 함께 crypto cycle, authentication failure, replay, sequence gap, queue overflow 및 session 통계를 보존하고, 이 결과를 release hot-path 실측과 동일하게 해석하지 않는다.

## 9. 이번 v3에서 구현하지 않는 강화 후보

다음 항목은 BLE 수준을 넘는 보안 강화 후보이지만 이번 작업에서는 **검토만 하고 구현하지 않는다**.

### 9.1 8-byte MIC

CCM tag를 8 byte로 늘리면 임의 위조 확률 상한이 한 번당 약 `2^-64`로 낮아진다. 현재 MIC4보다 packet당 4 byte가 늘며 2 Mbps에서 순수 payload airtime은 packet당 약 16 µs 증가한다. Crypto backend의 tag 처리 비용과 양쪽 동시 충돌 영향은 실측이 필요하다. MIC4 v3가 승인된 뒤 별도 profile에서만 검토한다.

### 9.2 ECDH와 forward secrecy

PSK-authenticated ECDH를 handshake에 추가하면 장기 PSK가 나중에 유출되어도 과거 session capture를 바로 복호화하기 어렵게 만들 수 있다. 그러나 ECC code/RAM, handshake 시간, 재부팅 복구와 protocol 상태가 늘어난다. ECDH만 추가하고 transcript를 PSK 또는 서명으로 인증하지 않으면 MITM을 막지 못한다. Steady-state packet 비용과 별개로 전체 신뢰성과 firmware footprint 검증이 필요하므로 이번 v3에는 넣지 않는다.

### 9.3 방향별 traffic key

하나의 session secret에서 uplink/downlink key를 별도로 파생하면 key separation을 강화할 수 있다. 이번 profile은 BLE 수준에 맞춰 하나의 link session key와 nonce direction domain을 사용한다. 방향별 key는 추가 key slot, RAM 및 KDF 구성을 측정한 뒤 검토한다.

### 9.4 Debug protection과 firmware authenticity

nRF52840의 APPROTECT, signed bootloader 및 firmware 서명은 runtime radio latency를 거의 늘리지 않을 수 있지만, 잘못 적용하면 field recovery와 개발용 SWD access를 잃거나 boot/update 구조를 바꿀 수 있다. 이는 무선 link 암호화와 별도의 supply-chain/physical-security 작업으로 분리하여 검토한다.

### 9.5 Root-control failure budget

`HELLO`/`RECOVERY`/`CHALLENGE`는 장기 PSK와 MIC4로 보호되지만, 잘못된 root tag 하나만으로 정상 active data link를 끊지는 않는다. 따라서 공격자가 root 형식 packet을 반복하면 software CCM 비용과 32-bit tag의 누적 시도 횟수가 늘어난다. Root-control에만 더 긴 MIC를 쓰거나 source별 token bucket/backoff와 전용 low-priority workqueue를 두면 정상 key data hot path의 packet 크기는 바꾸지 않고 이 위험을 줄일 가능성이 있다. 다만 공격 중 handshake/recovery 지연, queue 경합과 코드/RAM 증가를 실측해야 하므로 이번 profile에서는 구현하지 않고 별도 강화 후보로 남긴다.

### 9.6 인증 실패 경계의 무손실 복구

Application delivery ACK, 짧은 event journal과 authenticated key-state resync를 조합하면 MIC 실패 직후의 경계 tap까지 복구할 가능성이 있다. 그러나 event마다 추가 상태·ACK를 요구하면 최우선 조건인 fresh 1K 경로의 queueing과 airtime에 영향을 줄 수 있다. 이번 profile은 즉시 reset challenge와 기존 pre-session FIFO까지만 구현하고, 이 방식은 별도 성능 profile에서 v2/v3 p95·p99와 양쪽 동시 부하를 통과할 때만 검토한다.

## 10. 남는 보안 한계

v3가 설계대로 동작해도 다음 제한은 남는다.

- 32-bit MIC에는 누적 위조 확률 한계가 있다.
- RF jamming과 충돌로 인한 입력 지연/손실은 암호화로 막지 못한다.
- 공격자는 공개된 address/session header에 맞춘 잘못된 MIC를 보내 논리 link의 재협상을 유도하거나, root-control 형식의 packet을 반복해 software CCM과 system workqueue 시간을 소비시킬 수 있다. 전자는 BLE와 같은 fail-closed 정책의 가용성 대가이고 후자는 RF/CPU latency DoS 한계다. 인증 실패 순간의 in-flight key event는 유실될 수 있다.
- Root `RECOVERY` high-water는 RAM 상태이므로 dongle 재부팅을 가로질러 과거 root packet의 재생을 기억하지 않는다. 재생된 packet은 fresh dongle nonce의 challenge까지만 유도할 수 있고 새 traffic session을 완성하려면 half의 pending key 인증이 필요하지만, 재부팅 직후의 handshake/CPU 가용성 공격 표면은 남는다.
- Pending expiry는 단발 과거 packet의 영구 고착을 막지만, 공격자가 과거 root packet을 계속 재전송해 매 timeout마다 후보 경쟁을 반복하는 지속적 DoS는 막지 못한다. 이를 막는 강한 cross-reboot anti-replay에는 NVS에 저장한 monotonic epoch/ratchet이 필요하며 flash wear, 복구 절차와 latency를 별도 검증해야 하므로 이번 profile에는 넣지 않는다.
- Packet timing, 길이, source와 traffic 양에서 typing pattern 일부를 추론할 수 있다.
- Production UF2나 장치 flash에서 PSK를 추출한 공격자는 해당 link를 복호화하고 위조할 수 있다.
- 하나의 장기 PSK에서 session key를 도출하므로 forward secrecy가 없다.
- Relay 공격과 장치의 실제 물리적 위치는 인증하지 않는다.
- Random generator가 실패하거나 nonce uniqueness를 잃으면 CCM 보안이 깨질 수 있다.
- MIC 검증은 packet authenticity를 보장하지만 사용자의 의도나 keymap 동작 자체를 검증하지 않는다.

이 제한을 숨기기 위해 v3를 기존 BLE보다 “더 안전하다”고 포괄적으로 표현하지 않는다. v3의 주장은 실기 성능 gate를 통과한 뒤에도 “BLE 수준을 목표로 한 사전 공유 key 기반 ESB link 보호”로 한정한다.

## 11. 현재 검증 상태

| 항목 | 상태 |
|---|---|
| 보안 protocol과 위협 모델 | 문서화 완료 |
| AES-128-CCM/MIC4 firmware 구현 | 구현 완료, boot KAT 포함 |
| 양측 random handshake/session key | 구현 완료 |
| Cross-session replay 방지 | 구현 완료, strict monotonic sequence |
| Local production key provisioning | 구현 완료 |
| Public CI test-key profile | 구현 완료, `ci_only_*_testkey` 표기 |
| v3 left/right/dongle build | 미측정 |
| 기존 BLE/ESB v2 회귀 build | v3 변경 후 미측정 |
| CCM typical/p95/p99 처리 시간 | 미측정 |
| Secure fresh-event cadence | 미측정 |
| 양쪽 동시 100,000-event loss | 미측정 |
| USBPcap 1 ms cadence | 미측정 |
| End-to-end v2 대비 latency delta | 미측정 |
| Prospector 화면 및 battery 실기 | 미측정 |

구현 완료, CI compile/link 결과, descriptor 확인과 실기 성능 승인은 서로 다른 단계다. 표의 미측정 항목을 채우기 전에는 v3가 기존 1K급 반응성을 유지한다고 결론내리지 않는다. Production 빌드·플래시와 롤백 절차는 [Secure v3 빌드 및 플래시 문서](esb-v3-build-flash.md)를 따른다.
