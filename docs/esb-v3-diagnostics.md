# ESB v3 연결 실패 진단

화면은 켜지지만 양쪽 키가 입력되지 않을 때, 컴파일 성공만으로 무선 연결을 판정할 수 없다. 기존 v3 release는 일반 로그가 비활성화되어 초기화 실패가 USB 콘솔에 보이지 않을 수 있다.

`CONFIG_TOTEM_ESB_DIAGNOSTICS=y`는 초기화 결과와 5초 간격 카운터 요약을 기존 콘솔에 출력한다. 보안 정책·키·무선 주소·ACK/재시도·HFCLK 절전·마우스 설정·스택 설정을 바꾸지 않는다. 출력 자체가 실행 시간과 호출 스택에 영향을 주므로 성능 측정 및 일상 사용용은 아니다.

## 파일과 순서

공개 CI의 다음 세 파일은 같은 공개 테스트키를 사용하는 관측용 빌드다.

- `ci_only_totem_dongle_esb_v3_prospector_diagnostic_testkey.uf2`
- `ci_only_totem_left_esb_v3_diagnostic_testkey.uf2`
- `ci_only_totem_right_esb_v3_diagnostic_testkey.uf2`

처음에는 **동글만 diagnostic으로 교체**하고 양쪽은 기존 non-benchmark CI v3 testkey 파일을 유지해도 된다. 프로토콜과 키 세트가 같기 때문이다. 필요할 때만 양쪽도 diagnostic으로 바꾸고 각각 USB 데이터 케이블로 연결해 로그를 읽는다. BLE/v2/benchmark 파일을 섞지 않는다. 동일 세트 교체만으로 settings_reset을 반복할 필요는 없다.

현재 PC에서 동글의 콘솔은 COM10, Studio는 COM4로 관찰되었다. 포트 번호는 바뀔 수 있으므로 현재 장치 목록을 확인한다. 콘솔은 115200 baud, 8N1, DTR 활성으로 열며 최소 10초 읽는다. Studio 포트에 진단 명령이나 임의 데이터를 보내지 않는다.

부팅 초기에 USB 콘솔 버퍼가 가득 차면 상세 출력이 유실될 수 있다. 마지막 KAT 단계와 초기화 결과는 메모리에 남겨 주기적으로 다시 출력한다. 진단은 키·nonce·세션 ID·패킷 내용·키 입력 값을 출력하지 않는다.

## 해석

| 값 | 의미 |
| --- | --- |
| `crypto_init` | PSA 초기화 결과 |
| `crypto_kat` | AES-CCM/MIC4 및 CMAC 자체 검사 결과 |
| `crypto_roots` | 장치 역할에 맞는 루트 키 가져오기 결과 |
| `boot_nonce` | 주변 장치의 부팅 nonce 생성 결과. 동글은 `na` |
| `clock` | 초기 HFCLK 준비 결과 |
| `radio` | ESB radio/주소/RX 준비 결과 |
| `transport` | 중앙 또는 주변 장치 transport 초기화 결과 |
| `kat_step`, `kat_status` | 마지막 자체 검사 단계와 상태. 단계 1/8/10은 import errno, 나머지는 PSA status |

초기화 결과의 `0`은 성공, `na`는 아직 실행되지 않음, `-EINPROGRESS`는 시작했으나 결과가 기록되지 않음이다. 실패 수치는 해당 단계의 오류이며, 원문 키가 아니다. 모든 초기화가 성공해도 실제 무선 연결 성공을 의미하지는 않는다.

KAT 단계: 1=CCM 키 가져오기, 2=암호화, 3=정상 복호화, 4=암호문 변조 거부, 5=태그 변조 거부, 6=AAD 변조 거부, 7=nonce 변조 거부, 8=다른 키 가져오기, 9=다른 키 거부, 10=CMAC 키 가져오기, 11=CMAC 계산. 의도한 거부 검사는 PSA의 INVALID_SIGNATURE가 정상이다. 최종 성공 여부는 `crypto_kat=0`으로 판단한다.

카운터는 누적 관측값이다. 동글은 `rx → frame_ok → hello → ready`, 양쪽은 `tx_ok/tx_fail`, `rx → challenge → session_ok` 순서로 범위를 좁힌다. HELLO/READY/CHALLENGE/SESSION_OK는 인증 후 처리에 성공한 **수신** 횟수이며 중복 수신도 증가할 수 있다. 동글의 PRX ACK 성공은 TX_SPACE_AVAILABLE로 전달되어 `tx_ok=0`이어도 정상이다. 동글의 `ready>0`만으로 양쪽이 SESSION_OK를 받았다고 단정하지 않는다.

`frame_err`는 파서 호출 실패 횟수이며 RX 패킷 수와 일대일 대응하지 않는다. `last_frame_err`는 이후 성공해도 남아 있는 마지막 오류다. 여러 카운터는 ISR과 작업 스레드에서 각각 갱신되므로 한 줄이 완전히 같은 순간의 원자적 스냅샷은 아니다.

`tx_steps=done:result` 행은 각 API 완료 횟수와 마지막 반환값을 출력한다. `0:0`은 아직 관측되지 않음, 마지막 `-EINPROGRESS`는 진입 후 반환 기록이 아직 없음을 뜻한다. `send=...:0`은 소프트웨어 큐 접수, `write=...:0`은 SDK FIFO 접수, `start=...:0`은 SDK 시작 함수의 성공 반환이며 실제 무선 완료와는 다르다. 일반 송신과 재시도 모두 포함된다. `hf_request`의 0 이상은 요청 접수이고, 부팅 spinwait는 `hf_callback`에 집계되지 않는다. `hf_wait`와 `radio_busy`는 호출 시 대기 조건 관측 횟수이며 영구 장애를 뜻하지 않는다. 인터럽트에서는 숫자만 기록한다.

`sensor: device not ready.`는 고정 Prospector 모듈의 주변광 센서 APDS9960 메시지다. 이 문구 하나만으로 ESB 초기화 실패를 판정하지 않는다.

### 송신 중 정체 진단과 타이머 보완

2026-09-09 실제 왼쪽 정체 상태에서 20초 동안 `tx_ok=36903`,
`tx_fail=119`, `write=37023`, `start=37348`이 고정된 반면
`radio_busy`는 9750→11827, `send`는 46644→48721로 증가했다.
마지막 send 결과 -35는 이 펌웨어의 Zephyr errno 정의에서 **ENOMSG**이며
소프트웨어 대기열 포화를 뜻한다. Linux errno 번호로 해석하지 않는다.
HF-ready 검사를 통과한 뒤 SDK non-IDLE 분기로 계속 진입하므로,
대기열 펌프는 살아 있고 무선 transaction 완료가 정체된 범위로 좁혀진다.
포화가 처음 고장의 원인인지 결과인지의 시간 순서는 이 로그만으로 확정하지 않는다.

고정 SDK에는 Nordic의
[NCSDK-35742 수정](https://github.com/nrfconnect/sdk-nrf/commit/2a6a1bddbd5b1f342569ec11afc1dded8edfb898)이
누락되어 있었다. 현재 overlay는 첫 TX ramp-up의 COMPARE2 interrupt에서
STOP/CLEAR shortcuts를 해제하고 ACK 설정에서 경과 시간을 보존한다.
이로써 RADIO handler가 늦어질 때 같은 COMPARE2가 다시 타이머를 정지시키는
경로를 보완한다. COMPARE1 callback도 실제 COMPARE1 이벤트에서만 실행한다.
진행 중 payload, RF PID, 암호문 순서, queue 크기, 개인키는 변경하지 않는다.
실측 증상과 맞는 결함을 수정했지만, 고장 당시 SDK/하드웨어 상태는 읽지
못했으므로 실제 원인 확정이나 모든 지연 조건의 복구 보장은 아니다.

진단 빌드는 이제 5초마다 다음 읽기 전용 정보도 출력한다.

| 값 | 의미 |
| --- | --- |
| `sdk_state` | 고정 SDK enum: 0=IDLE, 1=PTX_TX, 2=PTX_TX_ACK, 3=PTX_RX_ACK, 4=PRX, 5=PRX_SEND_ACK, 6=PTX_TXIDLE |
| `radio_state` | RADIO STATE 하드웨어 레지스터의 원시 값 |
| `tx_queued`, `retries`, `irq_flags` | SDK FIFO 항목 수, 재시도 카운터 원시 값, 이벤트 flags(TX 성공 bit 0/실패 bit 1/RX bit 2) |
| `radio_events` | bit 0=READY, 1=ADDRESS, 2=END, 3=DISABLED |
| `timer_events` | bit 0=COMPARE0, 1=COMPARE1, 2=COMPARE2 |
| `timer_shorts` | TIMER SHORTS 레지스터의 원시 값 |
| `radio_irq`, `timer_irq` | bit 0=IRQ enabled, 1=pending |

숫자는 모두 10진수다. 함수는 lock, event clear, timer capture, 레지스터 쓰기를
하지 않는다. 따라서 IRQ가 항목 사이에 상태를 바꿀 수 있으며 단일 시점의
원자적 snapshot이 아니다. 이벤트 비트만으로 타이머가 현재 진행 중이라고
판정하지 않는다. SDK 초기화가 완료되기 전에는 이 행을 출력하지 않는다.
`retries`는 unsigned 후감소 값이므로 최종 실패 뒤 IDLE에서는 UINT32_MAX가
남을 수 있다. 그 값을 실제 남은 재시도 횟수로 해석하지 않는다.
출력에 패킷 내용, 키 위치, 개인키, nonce, session ID는 포함하지 않는다.

## 검증 범위

### 왼쪽 USB 인식 실패의 비교 빌드

`ci_only_totem_left_esb_v3_stackprobe_testkey.uf2`는 기존 왼쪽 diagnostic과 비교해 `CONFIG_MAIN_STACK_SIZE`만 1024에서 4096바이트로 늘린 진단 빌드다. 기존 diagnostic도 계속 제공한다. 암호화·키·주소·재시도·초기화 우선순위·USB 설정·마우스 설정을 변경하지 않는다. 스택 크기 증가에 따라 RAM 배치는 달라질 수 있다.

실제 기존 왼쪽 diagnostic의 ESB 초기화는 APPLICATION 우선순위 40, USB 활성화는 96이다. 암호화 자체 검사 및 부팅 nonce 생성이 USB 활성화보다 앞서 1024바이트 main 스택에서 실행된다. USB 장치 설명자 요청 실패(Windows 코드 43)만으로 이 경로의 스택 초과를 확정할 수는 없다. 비교 빌드는 동일한 부팅 순서에서 main 스택 여유만 늘려 USB 인식과 연결 결과를 관찰하기 위한 것이다.

이 파일은 왼쪽에만 올리고, 동글 diagnostic과 오른쪽 non-benchmark CI v3 testkey는 유지한다. 별도 settings_reset이나 업로드 순서 변경은 필요하지 않다. USB 인식이 회복되면 초기화 단계 및 TX/RX 로그를 수집한다. 결과가 개선되어도 스택 초과 자체의 확정이나 전체 연결 장애 해결로 단정하지 않는다.

### USB 콘솔을 먼저 여는 시작 진단

`ci_only_totem_left_esb_v3_usbstart_testkey.uf2`는 `CONFIG_TOTEM_ESB_DIAGNOSTIC_USB_START=y`를 추가한 별도 관측용 빌드다. 현재 v3 주변 장치는 USB 초기화 96 이후 SYS_INIT 99에서 생성한 4096바이트 전용 스레드로 ESB를 초기화한다. USBstart 옵션에서만 콘솔의 DTR을 기다린다. 포트를 열어 DTR이 켜지면 1초 후 시작 표시를 출력하고 기존 암호화·nonce·클럭·radio 초기화를 수행한다. 단계 출력 후 200ms 쉬어 USB가 로그를 전달할 시간을 확보한다.

이 빌드는 콘솔 대기와 초기화 타이밍을 변경하므로 원인 분리용이며 배터리만으로 쓰는 일상 펌웨어가 아니다. 암호화와 키 검사는 그대로 수행하며, 실패하면 무선 초기화를 계속하지 않는다. 현재 소스는 초기화 전 입력을 기존 유한 대기열에 보관한다. 오래 기다려 대기열이 가득 차면 짧은 탭이 유실될 수 있으므로 테스트할 키는 `transport_return ... result=0` 이후 새로 누른다. 콘솔을 열지 않으면 ESB도 시작하지 않는 것이 의도된 동작이다. 과거 `83c9936` USBstart 산출물은 초기화 전 입력을 거부하는 이전 진단 구현이다.

왼쪽만 이 파일로 바꾸고 USB로 PC에 연결해 둔다. 부팅 후 콘솔 포트가 생기면 DTR을 활성화해 최소 20초 로그를 읽는다. USB 콘솔이 여전히 나타나지 않으면 ESB 초기화에 들어가기 전의 문제로 범위를 좁힐 수 있다. USB가 열린 뒤 초기화 중 끊기면 마지막 전달된 단계부터 확인한다. 동작이 개선되어도 이 빌드를 원인 수정본으로 간주하지 않는다.

### 콘솔 대기 없는 v3 자동 시작

일반 v3 및 일반 diagnostic 주변 장치는 전용 초기화 스레드에서 즉시 시작하며, USB 콘솔·DTR·호스트 인식을 기다리지 않고 임의 지연도 넣지 않는다. v2 주변 장치는 기존 SYS_INIT 경로를 유지한다. 초기화가 끝나기 전에도 키 상태 bitmap을 갱신하고 입력 순서를 기존 `presession_events`에 보관하지만, 암호화·핸드셰이크·상태 전송 작업은 시작하지 않는다. 초기화가 성공하면 핸드셰이크를 시작하고 세션 확정 뒤 기존 순서대로 입력을 처리한다. 초기화 실패 시 평문으로 우회하지 않는다.

이 변경은 과거 SYS_INIT 40에서 실행하던 v3 초기화를 main 및 system workqueue와 분리한다. `83c9936` USBstart의 실기기 왼쪽 입력 성공이 근거지만, 그것은 순서·스레드·스택·진단 지연을 함께 변경한 결과다. 따라서 지연 없는 자동 시작은 별도로 USB 미연결 배터리 부팅과 양쪽 입력을 확인해야 한다.

호스트 모델과 C helper 테스트는 무선 칩·ARM PSA·전체 부팅·실제 ACK 교환을 실행하지 않는다. 진단 빌드의 컴파일 및 로그 형식 검증과, 장치의 실패 원인 확인은 별개다. 정확한 실패 원인은 해당 장치에 진단 빌드를 올린 후 수집한 로그로 결정한다.

### 초기화 시작 시점만 늦추는 비교

다음 파일은 각각 같은 쪽의 일반 diagnostic에 `CONFIG_TOTEM_ESB_DIAGNOSTIC_START_DELAY_MS=3000`만 추가한다.

- 왼쪽: `ci_only_totem_left_esb_v3_startdelay_testkey.uf2`
- 오른쪽: `ci_only_totem_right_esb_v3_startdelay_testkey.uf2`

같은 4096바이트 초기화 스레드에서 암호화·무선 초기화 전에 한 번만 3초 기다린다. USB 호스트나 DTR을 기다리지 않고 `UART_LINE_CTRL`도 켜지 않으며, 초기화 단계별 지연도 추가하지 않는다. 기본값 0인 일반 빌드는 즉시 시작한다. 오른쪽 비교도 peripheral ID 2, 오른쪽 테스트키, 재시도 간격 800과 기존 스택 설정을 유지한다. 빌드 후 같은 commit의 오른쪽 일반 diagnostic과 최종 `.config`를 비교해 지연값 0→3000 외의 차이가 없는지 확인한다.

오른쪽을 비교할 때는 오른쪽 파일만 교체하고 왼쪽과 동글의 현재 펌웨어를 유지한다. USB 연결 상태의 입력 및 일반 리셋부터 확인하고, 이후 배터리 전원 재시작과 동글 재시작 뒤 입력을 각각 확인한다. 왼쪽의 성공 결과를 오른쪽 결과로 간주하지 않는다.

USBstart는 DTR 대기 외에도 초기화 상태 기록마다 200ms를 쉰다. USB 초기화 함수가 반환되는 시점과 호스트의 USB 열거 완료는 다르므로, 전용 스레드만으로 문제가 완전히 해결됐다고 단정하지 않는다. 시작 전 대기 하나를 바꾼 이 빌드에서 USB 열거·실제 입력·일반 리셋 뒤 재연결이 달라지는지 확인한다. 성공해도 특정 암호화 함수나 클럭이 원인이라고 단정할 수 없으며, 실패해도 USBstart의 단계별 대기 차이가 남는다.

이 비교를 준비한 실물에서는 `35744b6` 자동 시작 왼쪽의 정상 입력 후 끊김과 리셋 뒤 USB 설명자 오류가 보고됐다. 부트로더 USB는 동작했고 `CURRENT.UF2`의 애플리케이션 107,264바이트는 제공한 자동 시작 UF2와 전부 일치했다. 이후 `5de80ea`의 3초 지연 왼쪽에서 첫 입력, USB를 유지한 일반 리셋 1회, 배터리 전원 재시작 1회, 동글 재시작 1회 뒤 입력 성공이 사용자에게 확인됐다. 이 제한된 관측은 오른쪽·반복 재시작·장시간 안정성이나 USB 오류의 근본 원인을 검증한 결과는 아니다.
