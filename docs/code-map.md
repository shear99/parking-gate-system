# 코드 지도

## 읽는 순서

1. `protocol/include/portfolio/gate/protocol.hpp`: `CommandRequest`, `CommandResult`, `BarrierState`, `GateOperation` 계약을 확인합니다.
2. `coordinator/src/coordinator_core.cpp`: `request_web_command()`가 웹 요청을 검증하고 중복 요청을 합치는 과정을 봅니다.
3. `edge-agent/src/agent_core.cpp`: `submit()`이 최종 판단을 하고 `on_a6_ack()`와 `on_rs232_frame()`이 서로 다른 결과를 처리하는 과정을 봅니다.
4. `edge-agent/src/a6_codec.cpp`: 문자열 프로토콜을 상태 타입으로 바꾸는 `decode_a6_frame()`을 봅니다.
5. `tests/`: 메모리에서 주입한 이벤트와 동시 요청으로 각 경계를 확인합니다.
6. `vision-pipeline.md`: 영상처리 원본의 검출·crop·OCR 연결을 확인합니다.

## 파일과 책임

| 경로 | 내용 |
|---|---|
| `protocol/include/`, `protocol/src/` | 요청·상태·이벤트 타입, JSON 직렬화, 시간 제한 등 계약 검증 |
| `coordinator/include/`, `coordinator/src/` | 웹 요청 합류·멱등성, 상태 snapshot, bounded 이벤트 보관 |
| `edge-agent/include/`, `edge-agent/src/` | Pi 최종 제어 판단, RS232 codec, 전송과 물리 동작 상태 |
| `tests/agent_core_test.cpp` | 상태 이벤트, ACK, 시간 제한과 동작 합류 |
| `tests/coordinator_core_test.cpp` | 웹 요청과 상태 갱신, 수동 동기화 경계 |
| `tests/concurrency_test.cpp` | 여러 요청이 동시에 도착할 때 단일 전송 판단 |

## 스냅샷 범위

- 기준은 기존 **Gate Control R6 C++ 구현**의 2026-10-10 로컬 소스입니다. 새 GateController 재작성이나 shadow 실험의 코드를 섞지 않았습니다.
- 선택한 코어와 기존 테스트를 옮겼으며, CMake는 해당 부분만 빌드하도록 작성했습니다.
- 공개용으로 C++ namespace·include 경로를 `portfolio::gate`와 `portfolio/gate`로 변경했습니다. site/gate 식별자는 `demo-site`, `demo-gate`로 바꿨습니다. 판단 알고리즘은 유지했습니다.
- 개인정보·운영주소가 들어갈 수 있는 설정, 배포 스크립트, 데이터, 로그와 Git 이력은 가져오지 않았습니다.
- 인증·권한 검사와 MQTT/HTTP 실행 어댑터는 이 스냅샷 밖의 계층입니다. 테스트 입력은 이미 해당 경계를 지난 가상의 요청입니다. 이 코어를 인증 없는 제어 API로 배포하는 예제를 제공하지 않습니다.
- A6 펌웨어는 포함하지 않습니다. `a6_codec.cpp`는 Pi 측 메시지 구성·해석 코드이며, 물리 통신 실행부가 아닙니다.
- 공개 파일의 SHA-256은 `snapshot-manifest.json`에 기록했습니다. 개인 운영 저장소의 전체 사본이나 재배포 패키지가 아닙니다.

## 빌드 의존성

C++20 표준 라이브러리, CMake, Threads, 외부 라이브러리 `nlohmann_json`만 사용합니다. 외부 라이브러리 소스와 모델 가중치는 이 저장소에 복사하지 않습니다.
