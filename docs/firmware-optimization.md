# 동작을 보존하는 펌웨어 최적화

현재 일반 이동은 기준값 2700/도달 시간 900ms의 2차 곡선을 사용하고, 500~900ms에 최대 1800의 2차 추가 가속으로 상한 4500에 도달한다. 직전 2400/900ms 설정보다 처음 500ms가 12.5% 빠르고 상한은 같다. Acc/E와 Prec/R은 가속 없는 절대 속도 15750/562.5로 움직인다. 모드 전환은 일반 가속 시작 시각을 바꾸지 않고 소수 이동량을 모드별로 보존한다. tap-preferred + hold-while-undecided 180ms, 콤보 50ms, 스크롤과 Base 복귀 규칙은 유지한다. [새 곡선](mouse-tuning.md)의 실물 사용감은 아직 검증 전이다.

## ESB 수신

동글은 실제 좌우 peer의 RX ring만 할당하고 예약 pipe 0의 저장소를 제거한다. 각 half는 자기 pipe의 RX ring 하나만 할당한다. 유효 pipe별 기존 용량을 줄이지 않으며 미할당 pipe는 수신 callback에서 거부한다. RX worker도 유효 pipe만 방문한다.

RADIO 수신 callback은 데이터가 유효한 동안 즉시 RX ring에 복사하므로, 그 전에 사용하던 중간 배열과 복사를 제거했다. 암호화·인증·재시도·입력 순서·스냅샷 복구 방식은 유지한다. 실제 메모리 절감량은 새 firmware의 linker 결과로 확인한다.

RF 설정은 기존 2Mbps, +8dBm 송신 출력, hopping 비활성화를 유지한다. 송신 출력을 올리거나 수신 감도를 변경하는 조정은 포함하지 않는다.

동글의 key-state 추적에 선행 눌림이 없는 wire release를 거르는 orphan-up 방어를 추가한다. 이런 release가 상대 이동 behavior로 전달되면 정지 상태에서 반대 방향 속도가 생길 수 있는 조건부 결함이 소스에서 확인됐다. Snapshot 합성 전이와 source 단절 해제는 기존 경로를 유지한다. 이 결함이 사용자가 보고한 간헐적인 약 1초 키 반복이나 고착의 실제 원인인지는 미확정이며, 해결 여부도 실물 검증 전이다. 상세 범위는 [입력 복구](esb-reliability-patch.md)를 참고한다.

ESB v3 peripheral은 producer ring이 가득 차도 기존 TX drain을 한 번 실행한 뒤 `-ENOSPC`를 반환한다. 하위 큐와 ring이 모두 찬 상태에서 일시적인 write/start 오류가 나면, 드라이버가 회복해도 신규 enqueue 실패만 반복하며 전송을 다시 시작하지 못하는 조건을 보완한다. 기존 프레임·순서·sequence는 유지하며 큐 초기화나 동기 재시도 루프는 추가하지 않는다. 실제 C를 실행하는 오류 주입 회귀에서 복구와 FIFO 보존, 지속 오류 시 호출 상한을 확인한다. 사용자가 보고한 한쪽 입력 단절의 실제 원인은 아직 미확정이며, HF callback 또는 RADIO busy가 계속되는 별도 조건까지 해결하는 변경은 아니다.

## Prospector 화면

WPM smoothing 작업을 초기화한 뒤 등록하고, 일반 system workqueue 대신 전용 display workqueue에서 실행한다. USB/ESB 출력 위젯도 이벤트 callback에서 직접 LVGL을 변경하지 않고 display listener로 전달한다.

배터리 위젯은 단일 source 이벤트 대신 모든 source의 배터리와 연결 상태를 snapshot으로 보존한다. 좌우 이벤트가 display queue에서 합쳐져도 두 상태가 남으며, 위젯 초기화 시 현재 상태를 읽는다. 값이 같은 재전달로 불필요한 표시·애니메이션을 반복하지 않는다. 새 세션에서 half가 배터리를 재전송하지 않아 동글 cache 자체가 비어 있는 문제는 별도이며, 이 표시 변경만으로 해결됐다고 보지 않는다.

실물에서 검증한 VDB25를 ESB 동글 기본값으로 올리고 이중 버퍼를 유지한다. `ram25` 아티팩트 이름은 기존 업로드 흐름을 위해 남기며, 이제 기본 diagnostic과 같은 display buffer 크기다. stack이나 입력 queue 용량을 줄인 것은 아니다.

기존에는 좌우 이벤트 누락을 우회하려고 100/500ms 뒤 상태를 다시 발행했다. 전체 상태 snapshot과 초기 cache 복원으로 이 우회를 대체하여 해당 작업과 중복 이벤트를 제거했다.

## 키맵과 Alt-Tab

Mouse 왼쪽 modifier hold가 단순 press/release만 전달하는 매크로 queue를 거치지 않고 직접 `kp`를 호출하도록 정리한다. 짧은 tap의 문자 입력과 Base 복귀는 기존 behavior를 사용한다.

owned swapper는 같은 owner의 중복 release에 불필요한 HID 재전송이나 retry 재예약을 하지 않는다. 정상 release 및 전송 실패 재시도, source 단절 시 해제, 무기한 latch와 기존 종료 조건은 유지한다. 이전 tri-state 외부 모듈은 더 이상 사용하지 않아 의존성에서 제거했다.

## 검증 범위

호스트 회귀는 실제 C 소스로 RX 저장소·pipe 선택·callback buffer 수명, 화면 listener의 초기화·이벤트 합쳐짐·실행 queue, Alt-Tab 중복 release를 검사한다. 후속 회귀는 곡선 helper의 초반 일치·부호·상한과 central wire dispatch의 orphan-up·snapshot·단절 처리를 검사한다. Zephyr scheduler, 실제 RADIO, USB와 LVGL 화면은 호스트 fake로 대체하므로 하드웨어 시험을 대신하지 않는다.

전체 BLE/v2/v3 빌드와 실제 linker 메모리 수치는 해당 커밋의 CI 결과로 확인한다. 새 최적화 펌웨어의 장치 검증은 업로드 후 입력·키 해제·재연결·WPM/배터리/레이어 표시를 확인해야 한다. 빌드 성공만으로 무선 지연이나 실사용 안정성 향상을 단정하지 않는다.

외부 모듈과 도구 고정 방식은 [의존성 관리](dependency-management.md)를 참고한다.
