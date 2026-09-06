# Totem + Prospector ESB Secure v3 빌드 및 플래시

Secure v3는 평문 ESB v2에 AES-128-CCM/MIC4, half별 PSK, session key와 replay 방지를 추가한 성능 후보 프로필이다. USB 1 ms 설정과 기존 ACK/retry는 유지하지만, 실제 장치에서 v2 대비 latency와 loss 승인 기준을 통과하기 전까지 “보안을 유지한 1K” 또는 일상용 권장 firmware로 간주하지 않는다.

## 1. 공개 CI artifact를 사용하지 않는 이유

`build.yaml`의 v3 항목은 기본·benchmark·진단 조합의 compile·link와 protocol 구성을 검증하기 위해 공개된 폐기용 test key를 사용한다. 파일명은 모두 `ci_only_..._testkey.uf2` 형식이며 다음 용도로만 쓴다.

- CI compile/link 회귀 확인
- flash/RAM 크기 확인
- 별도의 보안 가치가 필요 없는 시험 장치에서 protocol bring-up

공개 test key는 누구나 알고 있으므로 실제 키보드의 암호화·인증에 사용하면 안 된다. Production firmware는 아래 절차로 로컬에서 생성한 key를 넣어 빌드한다. Production `.keyconf`, UF2와 build directory를 GitHub, 메신저나 공개 artifact에 올리지 않는다.

## 2. 역할과 shield

| 역할 | shield 조합 | production key file |
|---|---|---|
| 왼쪽, peripheral ID 1 | `totem_left totem_esb_left totem_esb_v3` | `left.keyconf` |
| 오른쪽, peripheral ID 2 | `totem_right totem_esb_right totem_esb_v3` | `right.keyconf` |
| Prospector USB 동글 | `totem_dongle prospector_adapter totem_esb_dongle totem_esb_v3` | `dongle.keyconf` |

왼쪽 image에는 왼쪽 key만, 오른쪽 image에는 오른쪽 key만 들어간다. Dongle image에는 좌우 key가 모두 들어간다. 세 장치는 같은 key generation에서 나온 파일을 사용해야 한다.

## 3. Production key 생성

저장소 루트에서 Git에 포함되지 않는 외부 디렉터리를 정한 뒤 실행한다.

```powershell
$repo = (Resolve-Path .).Path
$keyDir = Join-Path $env:USERPROFILE 'totem-esb-v3-keys'

py -3 .\tools\generate_esb_v3_keys.py $keyDir
```

스크립트는 다음 세 파일을 만든다.

- `$keyDir\left.keyconf`
- `$keyDir\right.keyconf`
- `$keyDir\dongle.keyconf`

화면에는 key 자체가 아니라 좌우 SHA-256 64-bit fingerprint만 출력된다. Left fingerprint가 dongle의 left fingerprint와, right가 dongle의 right와 같은지 확인한다. 기존 파일은 기본적으로 덮어쓰지 않는다. `--force`로 교체하면 이전 firmware와 통신할 수 없으므로 세 image를 모두 다시 빌드하고 플래시해야 한다.

생성 후에는 읽기 전용 검증기로 역할별 키의 일치와 공개 테스트키 부재를 확인한다. 이 명령은 키를 생성하거나 교체하지 않으며, 실패하면 빌드 전에 원인을 확인한다.

```powershell
python "$repo\tools\verify_esb_v3_private_build.py" --key-dir "$keyDir"
if ($LASTEXITCODE -ne 0) { throw '개인키 세트 사전 검증이 실패했습니다.' }
```

검증기는 원문 키나 파일 내용을 출력하지 않고 검증 결과, 좌우 지문과 빌드 검사 시 파일 SHA-256만 출력한다. 키 파일·생성 헤더·바이너리 또는 빌드 로그 원문 대신 이 요약 결과만 공유한다.

Key file을 잃으면 기존 image에서 안전하게 복구하는 절차는 제공하지 않는다. 암호화된 오프라인 백업을 보관하되, key file과 production UF2가 유출되면 좌우 key를 모두 폐기하고 새 세트를 만든다.

## 4. 로컬 west workspace

ZMK가 요구하는 Zephyr SDK/toolchain, Python, Git과 `west`가 설치된 PowerShell을 사용한다. 저장소 밖의 별도 west workspace에서 작업하며, dependency와 build output을 원본 저장소에 섞지 않는다. 키와 production build output은 OneDrive 등 동기화 폴더 밖에 보관한다.

아래 명령은 **실제로 빌드할 수정 사항이 커밋된 로컬 저장소 루트**에서 시작한다. 기존 기반 브랜치를 다시 내려받으면 PR의 마우스 조정이 빠질 수 있으므로 현재 체크아웃을 복제하고 커밋 일치를 확인한다. 미커밋 변경이 있으면 먼저 정리한다. 새 workspace 경로를 사용하며 기존 workspace를 삭제하거나 덮어쓰지 않는다.

```powershell
$sourceRepo = (Resolve-Path .).Path
$sourceCommit = git -C $sourceRepo rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw '소스 저장소의 커밋을 확인하지 못했습니다.' }
$sourceChanges = git -C $sourceRepo status --porcelain
if ($LASTEXITCODE -ne 0 -or $sourceChanges) {
  throw '소스 저장소 상태를 확인하고 변경 사항을 커밋한 뒤 다시 실행하세요.'
}

$workspace = Join-Path $env:USERPROFILE 'zmk-esb-v3-workspace'
if (Test-Path -LiteralPath $workspace) { throw '새 workspace 경로를 지정하세요.' }
New-Item -ItemType Directory -Path $workspace | Out-Null
git clone --no-hardlinks -- $sourceRepo "$workspace\config"
if ($LASTEXITCODE -ne 0) { throw '소스 복제가 실패했습니다.' }
$copiedCommit = git -C "$workspace\config" rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $copiedCommit -ne $sourceCommit) {
  throw '복제된 소스 커밋이 원본과 다릅니다.'
}

Set-Location $workspace
west init -l --mf config/west.yml config
if ($LASTEXITCODE -ne 0) { throw 'west 초기화가 실패했습니다.' }
west update --fetch-opt=--filter=tree:0
if ($LASTEXITCODE -ne 0) { throw '의존성 준비가 실패했습니다.' }
west zephyr-export
if ($LASTEXITCODE -ne 0) { throw 'Zephyr 등록이 실패했습니다.' }

$repo = (Resolve-Path .\config).Path
$keyDir = Join-Path $env:USERPROFILE 'totem-esb-v3-keys'
```

저장소의 manifest는 루트의 `west.yml`이 아니라 `config/west.yml`이다. `--mf config/west.yml`을 생략하면 초기화가 실패한다. 위 구성에서 `$workspace\config`는 저장소 루트이며 `$repo\config`가 실제 ZMK 설정 폴더다.

이 저장소의 CMake compatibility layer는 configure 중 pinned ESB/Prospector checkout을 overlay한다. 같은 west workspace에서 여러 역할을 병렬 빌드하지 말고 아래 명령을 순차 실행한다. Production 빌드 로그와 `.config`에도 키가 포함될 수 있으므로 비공개 로컬 파일로 취급하고 원문을 공개하지 않는다.

Production 빌드에는 `CONFIG_TOTEM_ESB_V3_CI_TEST_KEYS=n`을 명시한다. 이 옵션만으로 입력한 키가 개인키임을 보장하지는 않는다. 빌드 전에 세 keyconf의 키가 32자리 16진수인지, 왼쪽·오른쪽 키가 서로 다르고 각각 동글의 대응 키와 일치하는지, 공개 테스트키와 다른지 확인한다. 원문 대신 일치 여부나 fingerprint만 기록한다. 키 파일 일부만 존재하면 새 세트를 자동 생성하거나 기존 파일을 덮어쓰지 않는다.

## 5. 검증 설정을 유지한 개인키 빌드

아래 명령은 `fc3e326`의 성공한 CI 조합인 좌우 `lowprioprobe`와 동글 `ram25`의 설정을 유지하고, 공개 테스트키 대신 역할별 개인키를 주입한다. CI 빌드 성공과 개인키로 빌드한 실물의 연결 검증은 별개다. 이 단계에서는 마우스 속도·키맵이나 진단 설정을 함께 바꾸지 않는다.

| 유지할 설정 | 왼쪽·오른쪽 | 동글 |
|---|---|---|
| ESB 진단 | `TOTEM_ESB_DIAGNOSTICS=y` | `TOTEM_ESB_DIAGNOSTICS=y` |
| low-priority workqueue stack | `4096` bytes | 기존 `768` bytes |
| 부팅 시 ESB 시작 지연 | `3000` ms, USB 콘솔 대기 없음 | 해당 없음 |
| LVGL 화면 버퍼 | 해당 없음 | VDB `25`, double buffer 유지 |
| 개인키 입력 | 각 half의 `.keyconf` | 양쪽 키가 들어간 `dongle.keyconf` |

`TOTEM_ESB_DIAGNOSTIC_START_DELAY_MS`는 진단 기능을 켠 v3 peripheral에서만 적용된다. 따라서 여기서 `DIAGNOSTICS=n`으로만 바꾸면 검증한 3000 ms 시작 지연도 유지되지 않는다. 현재 진단은 초기화 결과와 집계 카운터를 출력하며 키나 packet 내용은 출력하지 않지만, 출력 부하가 있으므로 진단 제거는 별도의 비교 빌드로 검증한다. 두 half의 low-priority stack 4096과 별도 ESB 시작 thread의 stack 4096은 서로 다른 설정이다.

### 왼쪽

```powershell
west build -p always -s zmk/app -d build/totem_left_esb_v3 -b "xiao_ble//zmk" -- `
  "-DSHIELD=totem_left totem_esb_left totem_esb_v3" `
  "-DCONFIG_TOTEM_ESB_V3_CI_TEST_KEYS=n" `
  "-DCONFIG_TOTEM_ESB_DIAGNOSTICS=y" `
  "-DCONFIG_TOTEM_ESB_DIAGNOSTIC_START_DELAY_MS=3000" `
  "-DCONFIG_TOTEM_ESB_DIAGNOSTIC_USB_START=n" `
  "-DCONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=4096" `
  "-DEXTRA_CONF_FILE=$keyDir\left.keyconf" `
  "-DZMK_CONFIG=$repo\config" `
  "-DZMK_EXTRA_MODULES=$repo"
if ($LASTEXITCODE -ne 0) { throw '왼쪽 개인키 빌드가 실패했습니다.' }
python "$repo\tools\verify_esb_v3_private_build.py" --key-dir "$keyDir" `
  --role left --build-dir build/totem_left_esb_v3
if ($LASTEXITCODE -ne 0) { throw '왼쪽 개인키 빌드 검증이 실패했습니다.' }
```

결과: `build/totem_left_esb_v3/zephyr/zmk.uf2`

### 오른쪽

```powershell
west build -p always -s zmk/app -d build/totem_right_esb_v3 -b "xiao_ble//zmk" -- `
  "-DSHIELD=totem_right totem_esb_right totem_esb_v3" `
  "-DCONFIG_TOTEM_ESB_V3_CI_TEST_KEYS=n" `
  "-DCONFIG_TOTEM_ESB_DIAGNOSTICS=y" `
  "-DCONFIG_TOTEM_ESB_DIAGNOSTIC_START_DELAY_MS=3000" `
  "-DCONFIG_TOTEM_ESB_DIAGNOSTIC_USB_START=n" `
  "-DCONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=4096" `
  "-DEXTRA_CONF_FILE=$keyDir\right.keyconf" `
  "-DZMK_CONFIG=$repo\config" `
  "-DZMK_EXTRA_MODULES=$repo"
if ($LASTEXITCODE -ne 0) { throw '오른쪽 개인키 빌드가 실패했습니다.' }
python "$repo\tools\verify_esb_v3_private_build.py" --key-dir "$keyDir" `
  --role right --build-dir build/totem_right_esb_v3
if ($LASTEXITCODE -ne 0) { throw '오른쪽 개인키 빌드 검증이 실패했습니다.' }
```

결과: `build/totem_right_esb_v3/zephyr/zmk.uf2`

### Prospector USB 동글

```powershell
west build -p always -s zmk/app -d build/totem_dongle_esb_v3_prospector `
  -b "xiao_ble//zmk" -S studio-rpc-usb-uart -- `
  "-DSHIELD=totem_dongle prospector_adapter totem_esb_dongle totem_esb_v3" `
  "-DCONFIG_TOTEM_ESB_V3_CI_TEST_KEYS=n" `
  "-DCONFIG_TOTEM_ESB_DIAGNOSTICS=y" `
  "-DCONFIG_LV_Z_VDB_SIZE=25" `
  "-DCONFIG_LV_Z_DOUBLE_VDB=y" `
  "-DEXTRA_CONF_FILE=$keyDir\dongle.keyconf" `
  "-DCONFIG_ZMK_STUDIO=y" `
  "-DZMK_CONFIG=$repo\config" `
  "-DZMK_EXTRA_MODULES=$repo"
if ($LASTEXITCODE -ne 0) { throw '동글 개인키 빌드가 실패했습니다.' }
python "$repo\tools\verify_esb_v3_private_build.py" --key-dir "$keyDir" `
  --role dongle --build-dir build/totem_dongle_esb_v3_prospector
if ($LASTEXITCODE -ne 0) { throw '동글 개인키 빌드 검증이 실패했습니다.' }
```

결과: `build/totem_dongle_esb_v3_prospector/zephyr/zmk.uf2`

각 build 후 검증기는 `.config`, 생성 키 헤더, UF2의 역할·키·영역·진단 표식과, 존재하는 경우 `zmk.bin`과의 일치를 검사한다. 소스·도구 체인 확인이나 실제 장치 설치·RF 연결 시험을 대신하지는 않는다. 다음은 비공개 로컬 환경에서 확인할 `.config`의 공통 값이다.

```text
CONFIG_TOTEM_ESB_V3=y
# CONFIG_TOTEM_ESB_V3_CI_TEST_KEYS is not set
CONFIG_ZMK_SPLIT_ESB=y
CONFIG_ESB_MAX_PAYLOAD_LENGTH=64
CONFIG_TOTEM_ESB_DIAGNOSTICS=y
```

왼쪽·오른쪽에서는 다음 값도 확인한다.

```text
CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=4096
CONFIG_TOTEM_ESB_DIAGNOSTIC_START_DELAY_MS=3000
# CONFIG_TOTEM_ESB_DIAGNOSTIC_USB_START is not set
```

동글에서는 다음 값도 확인한다.

```text
CONFIG_ZMK_STUDIO=y
CONFIG_USB_HID_POLL_INTERVAL_MS=1
CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT=2
CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=768
CONFIG_LV_Z_VDB_SIZE=25
CONFIG_LV_Z_DOUBLE_VDB=y
CONFIG_LV_Z_BUFFER_ALLOC_STATIC=y
```

`zephyr/zephyr.dts`에서도 Mouse 이동값 1600, 최대 속도 도달 시간 490 ms, E/R tap-preferred·hold-while-undecided·180 ms가 유지됐는지 확인한다. 콤보는 기존 조합과 기본 50 ms를 유지한다. 공개 테스트키 옵션과 개인키 입력을 제외한 CONFIG 차이가 위 비교 대상과 다르면 원인을 확인한 뒤 업로드한다. CI와 개인키 빌드의 UF2 hash가 같아야 하는 것은 아니다.

Production key 문자열을 log에 출력하거나 `.config`를 공개하지 않는다. 생성 헤더·ELF·object·UF2에도 키가 들어가므로 build 폴더 전체를 비공개로 보관한다. Windows에서는 파일의 쓰기 속성만으로 접근 제어가 보장되지 않으므로 키 폴더의 ACL도 확인한다.

## 6. Secure benchmark 빌드

아래는 5절의 연결 검증용 조합과 다른 성능 측정 프로필이다. 역할별 shield에 `totem_esb_benchmark`를 추가하고 같은 production key file을 전달한다. Benchmark는 0 ms debounce, RTT logging, 1,000 µs synthetic producer와 security/crypto 통계를 켜므로 일상 사용용이 아니다. Dongle benchmark에는 Studio RPC/CDC를 넣지 않는다.

```powershell
west build -p always -s zmk/app -d build/totem_left_esb_v3_benchmark `
  -b "xiao_ble//zmk" -- `
  "-DSHIELD=totem_left totem_esb_left totem_esb_v3 totem_esb_benchmark" `
  "-DCONFIG_TOTEM_ESB_V3_CI_TEST_KEYS=n" `
  "-DEXTRA_CONF_FILE=$keyDir\left.keyconf" `
  "-DZMK_CONFIG=$repo\config" "-DZMK_EXTRA_MODULES=$repo"

west build -p always -s zmk/app -d build/totem_right_esb_v3_benchmark `
  -b "xiao_ble//zmk" -- `
  "-DSHIELD=totem_right totem_esb_right totem_esb_v3 totem_esb_benchmark" `
  "-DCONFIG_TOTEM_ESB_V3_CI_TEST_KEYS=n" `
  "-DEXTRA_CONF_FILE=$keyDir\right.keyconf" `
  "-DZMK_CONFIG=$repo\config" "-DZMK_EXTRA_MODULES=$repo"

west build -p always -s zmk/app -d build/totem_dongle_esb_v3_prospector_benchmark `
  -b "xiao_ble//zmk" -- `
  "-DSHIELD=totem_dongle prospector_adapter totem_esb_dongle totem_esb_v3 totem_esb_benchmark" `
  "-DCONFIG_TOTEM_ESB_V3_CI_TEST_KEYS=n" `
  "-DEXTRA_CONF_FILE=$keyDir\dongle.keyconf" `
  "-DZMK_CONFIG=$repo\config" "-DZMK_EXTRA_MODULES=$repo"
```

측정과 분석은 [ESB 1K benchmark 문서](esb-1k-benchmark.md)를 따르고, v3에서는 `BENCH_CRYPTO`, authentication/replay/session drop 통계도 함께 보존한다.

## 7. 플래시 순서

1. 현재 정상 동작하는 UF2 세트를 백업하고, 기존 개인키 v3 세트가 있다면 그 key file도 별도로 백업한다.
2. Prospector 동글을 bootloader mode로 열고 개인키 v3 dongle UF2를 플래시한다.
3. 왼쪽에 같은 key set의 left UF2를 플래시한다.
4. 오른쪽에 같은 key set의 right UF2를 플래시한다.
5. 세 장치를 재부팅한다. Half는 ESB 시작 전 3초를 기다리므로 충분히 기다린 뒤 동글 USB enumerate, Prospector 화면, 좌우 연결·battery와 모든 key release를 확인한다. 각 half 전원 재시작과 동글 USB 재연결 후에도 다시 입력되는지 확인한다.

동글→왼쪽→오른쪽 순서는 역할과 키 세트를 혼동하지 않기 위한 작업 순서이며, 프로토콜의 필수 플래시 순서는 아니다. 공개 테스트키에서 개인키로 바꾸는 동안에는 아직 다른 키를 쓰는 장치가 연결되지 않는 것이 정상이다. 세 장치의 업로드가 모두 끝난 뒤 판단한다.

**일반 v3 업데이트와 키 교체에 `settings_reset`은 필요하지 않다.** 현재 PSK는 빌드된 [생성 헤더](../CMakeLists.txt)에서 로드하고, [암호 구현](../src/totem_esb_v3_crypto.c)은 PSA 키를 volatile로 import한다. [Peripheral](../compat/zmk-feature-split-esb/src/split/esb/peripheral.c)은 부팅마다 새 random nonce로 handshake를 시작하고, [central](../compat/zmk-feature-split-esb/src/split/esb/central.c)의 session/replay 상태도 RAM에 둔다. ESB v3의 연결 키나 session을 settings에 저장·복원하는 경로가 없어 reset firmware를 먼저 넣을 이유가 없다. 기존 연결·재시작 실물 확인도 reset firmware 없이 진행했다.

`settings_reset`은 BLE bond나 저장된 Studio 설정 등을 의도적으로 초기화할 때 사용한다. Pinned ZMK의 [NVS reset 구현](https://github.com/zmkfirmware/zmk/blob/904c9aec8822d79149d42c8a9a77e8828eb08f5a/app/src/settings/reset_settings_nvs.c)은 settings flash partition 전체를 지우므로, 현재 설정을 유지하려는 업데이트에 반복 적용하지 않는다. 키 불일치는 reset으로 해결되지 않으며 올바른 동일 세트의 세 UF2를 다시 빌드·업로드해야 한다.

다른 generation의 v3 key file, v2와 v3 또는 공개 CI test-key image를 섞으면 정상 session이 성립하지 않는다. 인증되지 않은 packet을 평문으로 받아들이는 fallback은 없다.

Active/pending traffic에서 MIC 인증 실패가 발생하면 v3는 BLE와 같은 fail-closed 원칙으로 해당 session을 폐기하고 fresh handshake를 수행한다. Dongle은 폐기한 session에 묶인 인증된 reset challenge를 즉시 ACK 대기열에 넣고, 실패하면 기존 250 ms root recovery heartbeat를 fallback으로 사용한다. Root recovery sequence는 같은 half boot nonce의 traffic rekey와 timeout을 지나도 감소하지 않아 늦거나 재생된 구 session heartbeat가 반복 rekey를 만들지 않는다. ACK FIFO에 들어간 pending challenge도 `READY`까지 고정해 다음 heartbeat와 key가 엇갈리지 않게 한다. Reset pending과 일반 pending은 2초 안에 확인되지 않으면 폐기되고, `SESSION_OK`가 2초 동안 오지 않으면 half도 새 nonce의 `HELLO`로 복구한다. 이때 Prospector의 해당 half 연결 표시가 잠시 끊기고 다시 연결될 수 있으며, 실패 시점에 이미 전송 중이던 경계 key event는 보안상 수락되지 않아 유실될 수 있다. 반복된다면 key set 불일치, RF 간섭, 손상된 image 또는 의도적인 무선 공격을 의심하고 v2로 롤백해 원인을 분리한다.

## 8. 실사용 승인 전 측정

같은 장치·USB port·RF 위치에서 v2와 v3를 교대로 측정한다.

- USBPcap에서 interrupt-IN `bInterval=1`과 실제 1 ms service cadence
- Left-only, right-only, 양쪽 동시 fresh sequence cadence
- Source별 100,000 synthetic event에서 loss·duplicate·replay·queue overflow
- AES-CCM seal/open typical·p95·p99
- v2 대비 key detection→host latency delta
- Prospector display on 상태와 좌우 battery/peer 표시
- RF 혼잡 조건의 retry와 terminal failure

승인 목표는 crypto p99 100 µs 이하, 동글 양쪽 crypto 합계가 1 ms window의 25% 이하, v2 대비 end-to-end p95/p99 증가 0.25 ms 이하, source별 loss 0/100,000이다. 모두 측정 목표이며 실측 전에는 달성값이 아니다. 하나라도 실패하면 v3를 일상용으로 채택하지 않고 검증된 v2로 롤백한다. MIC 제거, ACK/retry 축소나 평문 fallback으로 수치만 맞추지 않는다.

## 9. 롤백

### 평문 ESB v2

다음 v2 release를 세 장치의 역할에 맞게 플래시하고 재부팅한다. v3 session은 RAM 상태이므로 v2로 전환하기 위해 `settings_reset`을 먼저 적용할 필요는 없다.

- `totem_left_esb.uf2`
- `totem_right_esb.uf2`
- `totem_dongle_esb_prospector.uf2`

### 기존 BLE split

다음 기존 BLE split 파일을 세 장치의 역할에 맞게 플래시한다. 저장된 BLE bond가 맞으면 기존 연결을 사용할 수 있다. Bond 불일치로 새로 페어링해야 하거나 저장 설정을 초기화하려는 경우에만 세 장치에 `settings_reset-xiao_ble__zmk-zmk.uf2`를 적용한 뒤 BLE firmware를 다시 넣고 재페어링한다.

- `totem_left-xiao_ble__zmk-zmk.uf2`
- `totem_right-xiao_ble__zmk-zmk.uf2`
- `totem_dongle prospector_adapter-xiao_ble__zmk-zmk.uf2`

v3 key file은 BLE/v2에서 사용하지 않지만 향후 같은 v3 세트로 돌아갈 계획이면 안전하게 보관한다.
