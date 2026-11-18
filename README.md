# Open Wake Switch

ESP32 + 서보로 기숙사 벽 스위치를 정해진 시간에 켜는 장치.
폰 웹앱 ↔ EMQX Cloud(MQTT/TLS) ↔ ESP32(GTother).

## 폴더 구성
- `firmware/open_wake_switch/` : Arduino 스케치 (폴더째로 열기)
- `webapp/index.html` : 폰용 웹앱 (Netlify / GitHub Pages에 그대로 올리기)
- `tests/` : PC에서 돌리는 알람 로직·웹앱 테스트

## 펌웨어 올리기
1. 라이브러리 설치: **ESP32Servo**, **PubSubClient**(Nick O'Leary), **ArduinoJson**(v7)
2. `secrets_example.h`를 복사해서 `secrets.h`로 이름 바꾸고 채우기
3. (권장) `ca_cert.h`에 EMQX 콘솔에서 받은 CA 인증서 붙여넣기
4. 보드: ESP32 Wrover Module → 업로드 → 시리얼 모니터 115200

시리얼에 보여야 하는 것:
```
[wifi] MAC ...
[mqtt] connecting... OK
[time] synced: 2026-11-18T...
```

## 웹앱
- 처음 열면 브로커 주소 / `phone` 계정 / 비밀번호 입력 → 폰에만 저장됨
- 코드 안에 비밀번호 없음 → 공개 저장소에 올려도 됨

## MQTT 토픽
| 토픽 | 방향 | 내용 |
|---|---|---|
| `ows/cmd` | 웹→기기 | `ON` / `OFF` |
| `ows/config/set` | 웹→기기 | 전체 설정 JSON (retained, 기기가 적용 후 지움) |
| `ows/config` | 기기→웹 | 기기에 실제 저장된 설정 (retained) |
| `ows/state` | 기기→웹 | 다음 알람, 마지막 동작, 시간 동기화 상태 (retained) |
| `ows/online` | 기기→웹 | `1` / `0` (Last Will) |
| `ows/error` | 기기→웹 | 설정 거부 사유 |

## 알람 규칙
- 요일별 시각 (0=일 … 6=토), 요일 알람 마스터 스위치
- 이번만 변경: 다음 요일 알람 하루만 다른 시간 또는 건너뛰기 (그날이 지나면 자동 삭제)
- 일회성 알람: 최대 5개, 울린 뒤 자동 삭제 (마스터 스위치와 무관하게 동작)
- 자동 끄기: 알람으로 켠 뒤 N분 후 끔 (기본 꺼짐)
- 놓친 알람: 재부팅 등으로 늦어도 허용 시간(기본 10분) 이내면 실행
- 시간 동기화 전에는 절대 동작하지 않음, 같은 알람 두 번 실행 안 함
- 설정을 저장한 순간 이전 시각의 알람은 처리된 것으로 간주 (방금 지난 시간으로 바꿔도 바로 켜지지 않음)
