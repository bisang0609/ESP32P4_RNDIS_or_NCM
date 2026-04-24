# AI Handoff

목적: 다른 PC/대화창에서도 지금 상태를 그대로 이어서 작업하기 위한 인수인계 문서

## 1) 세션 메타
- 프로젝트: ESP32P4_RNDIS_or_NCM
- 최종 업데이트(KST): 2026-04-24 17:52
- 브랜치: YTMD
- HEAD: e056f64 (`이미지리소스정리_플레이어UI개선`)
- 타겟/SDK: esp32p4 / ESP-IDF v6.0
- 워킹트리 상태: 변경 있음 (커밋 전)
- 작업/검증 범위: 빌드 

## 2) 현재 목표
- 목표: 앨범아트 전환 지연 감소 + `next_song`(다음곡 타이틀/가수 라벨) 정확도 개선
- 현재 이슈: `/queue` 파싱 결과가 실제 다음곡과 다른 케이스 존재

## 3) 이번까지 완료된 핵심 작업
- clean+build 시작 스크립트 추가
  - `tools/start_clean_build.ps1`
  - `-ResetManagedComponents` 옵션 지원
- 폴링 주기 단축
  - `YTMD_POLL_INTERVAL_MS: 2000 -> 500`
- 앨범아트 프리패치 비동기화
  - prefetch worker task + 요청 coalescing
  - cache lock(뮤텍스) 추가
- album task 즉시 깨우기 경로 추가
  - `ulTaskNotifyTake` 기반 대기
  - next/prev 명령 후 즉시 refresh notify
- 보조 상태(enrich) 주기 분리
  - `YTMD_AUX_STATE_ENRICH_INTERVAL_MS = 1500`
- `next_song` 라벨 경로 개선 (핵심)
  - `/song` 수신 시 현재곡 힌트(title/artist/videoId) 저장
  - `/queue` 파싱 시 리스트를 먼저 끝까지 구성
  - 중복 renderer(동일 videoId/동일 row+텍스트) 제거
  - `selected` 미검출 시 힌트 매칭으로 현재 인덱스 복원
  - next 선택 시 `selected+1`만 쓰지 않고
    `selected row index`보다 큰 항목 중 최소 row 우선 선택
  - 디버그 로그 추가:
    - `NEXT: queue-cache resolve selected_pos=... count=... hint_title=... hint_artist=... hint_vid=...`

## 4) 현재 남은 검증 포인트
- 사용자 보고:
  - `NEXT: parsed from /queue => '...'`
- 조치:
  - 위 파서/선택 로직을 재설계하여 반영 완료
- 미완료 이유:
  - 실기기 업로드가 COM26 점유로 실패하여 현장 검증 미진행

## 5) 빌드/업로드 결과
- build: 여러 차례 성공
- app-flash: 실패
  - 에러: `Could not open COM26` / `PermissionError(13, access denied)`

## 6) 주요 파일 변경 (현재 세션)
- `main/ytmd_client.c`
  - next_song 파서/큐 캐시/힌트 매칭/선택 로직 개선
  - next 명령 fallback 경로 보강
- `main/ytmd_client.h`
  - poll interval 및 신규 API 선언 반영
- `main/main.c`
  - album task notify 기반 대기/즉시 refresh 경로
  - UI control ops(prev/next) 커스텀 연결
- `tools/start_clean_build.ps1` (신규)
- `AI_HANDOFF.md` (본 문서)

## 7) 다음 세션 즉시 할 일
1. COM26 점유 해제 후 `app-flash` 성공시킴
2. next_song 검증 로그 2줄 확보
   - `NEXT: queue-cache resolve ...`
  - `NEXT: parsed from /queue => '...'`
3. 실제 UI `next_song` 라벨과 로그를 대조
4. 여전히 불일치 시 `/queue` payload에서 selected/row/videoId 샘플 캡처 후 매칭 규칙 2차 보정

## 8) 자주 쓰는 명령
```bash
# 시작 준비: clean + build
powershell -ExecutionPolicy Bypass -File .\tools\start_clean_build.ps1

# managed_components까지 초기화가 필요할 때
powershell -ExecutionPolicy Bypass -File .\tools\start_clean_build.ps1 -ResetManagedComponents

# 빌드
. C:\esp\v6.0\esp-idf\export.ps1; idf.py -B build_DEV_TEAM_2 build

# 업로드
. C:\esp\v6.0\esp-idf\export.ps1; idf.py -B build_DEV_TEAM_2 -p COM26 app-flash
```

## 9) 다음 세션 시작 프롬프트 (복붙용)
"AI_HANDOFF.md 기준으로 이어서 진행. COM26 점유 해제 후 app-flash부터 하고, next_song 라벨이 실제 다음곡과 일치하는지 로그(`queue-cache resolve`, `parsed from /queue`) 기준으로 검증해줘."
