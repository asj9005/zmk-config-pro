# 고정 빌드 환경과 외부 모듈 관리

`config/west.yml`은 ZMK, Zephyr, ESB, Nordic SDK/암호화와 Prospector의 커밋 SHA를 고정한다. 포크는 본인 계정에서 수정·보관하는 방법이며, 포크 자체가 버전을 고정하지는 않는다. 포크를 사용할 때도 `revision`에는 검증한 커밋 SHA를 넣는다.

현재는 전체 모듈을 복사하지 않고, 실제 수정하는 파일만 `compat/`에 원본 라이선스를 유지하며 보관한다. 빌드 시 지정된 외부 모듈의 파일에 적용한다. 자체 키맵 동작은 `src/`와 `dts/`에 있다. 따라서 키맵·외부 수정 파일·그 수정에 맞는 의존성 버전을 한 PR에서 검토할 수 있다. 향후 ESB나 Prospector를 여러 키보드에서 공동 관리하려면 해당 모듈만 `asj9005` 계정으로 포크하는 것이 적절하다.

| 의존성 | 관리 방식 |
| --- | --- |
| ZMK / Zephyr | upstream 커밋 SHA 고정 |
| zmk-feature-split-esb | 커밋 SHA 고정 + `compat/zmk-feature-split-esb` |
| sdk-nrf | 커밋 SHA 고정 + per-pipe ACK 수정 파일 |
| nrfxlib / mbedtls / oberon-psa-crypto | 서로 맞는 커밋 SHA 고정 |
| Prospector | 커밋 SHA 고정 + ESB 표시·작업 큐 수정 파일 |
| 이전 zmk-tri-state | 자체 owned swapper로 대체되어 의존성에서 제거 |

`CMakeLists.txt`의 overlay는 외부 checkout을 수정한다. 같은 west workspace에서 서로 다른 구성을 동시에 빌드하지 않는다. CI는 job별 workspace를 사용한다. 로컬에서도 구성 변경은 순차 빌드하거나 별도 workspace를 사용한다. ESB용 Prospector/Nordic 수정은 BLE 구성 시 복원한다.

## 빌드 도구 고정

`.github/workflows/build-pinned.yml`은 ZMK의 `904c9aec8822d79149d42c8a9a77e8828eb08f5a` reusable workflow를 가져와 이 저장소에서 관리한다. 성공한 run `34016341940`에서 사용한 다음 버전을 고정했다.

- 빌드 이미지: `zmkfirmware/zmk-build-arm@sha256:edb1c953438c6f720ddb79c3762f3972013b7fbbaf4fff3592fc869983e7afc5`
- Zephyr SDK 0.16.9, CMake 3.31.6, Python 3.12.3: 위 이미지의 성공 로그 기준
- checkout: `3d3c42e5aac5ba805825da76410c181273ba90b1` (v7, Node.js 24)
- cache: `caa296126883cff596d87d8935842f9db880ef25` (v5)
- upload-artifact 및 merge: `043fb46d1a93c77aae656e7c1c64a875d1fc6a0a` (v7)

새 CI는 `west manifest --freeze`와 도구 버전을 로그에 남긴다. GitHub 호스트 runner 자체와 외부 저장소의 가용성까지 영구 고정하거나 UF2의 bit-for-bit 재현성을 증명한 것은 아니다. 호스트 runner 이미지·실제 의존성 revision·생성된 설정과 파일 해시는 성공한 각 빌드에서 확인한다.

## 개인키 빌드

공개 CI는 공개 테스트키 빌드 전용이다. Kconfig와 devicetree를 로그로 출력하고 UF2를 아티팩트에 저장하므로 개인키를 CI에 전달하지 않는다. 개인키 설정과 생성된 `.config`, 헤더, ELF, UF2는 모두 비공개 로컬 경로에 보관한다.

지금까지 검증한 좌우 연결 설정은 low-priority stack 4096바이트와 시작 지연 3000ms이다. 일반 좌우 프로필의 기본값이 모두 이 값으로 승격된 것은 아니므로 최종 로컬 빌드에서 명시적으로 보존한다. 시작 지연은 현재 `TOTEM_ESB_DIAGNOSTICS=y`에 종속된다. 동글 VDB25는 기본 ESB 동글 프로필에 반영했으며 이중 버퍼는 유지한다.

관련 문서: [동작·메모리 최적화](firmware-optimization.md), [개인키 빌드](esb-v3-build-flash.md), [Alt-Tab 동작](owned-swapper.md).
