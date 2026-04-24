# AI Handoff

목적: 다른 PC/대화창에서도 지금 상태를 그대로 이어서 작업하기 위한 인수인계 문서

## 1) 세션 메타
- 프로젝트: ESP32P4_RNDIS_or_NCM
- 최종 업데이트(KST): 2026-04-24 22:38
- 브랜치: YTMD
- HEAD: a116b50 (`feat: finalize next-song parsing and seekbar timeline UX`)
- 타겟/SDK: esp32p4 / ESP-IDF v6.0
- 워킹트리 상태: 변경 있음 (`AI_HANDOFF.md` 갱신본, 커밋 전)
- 작업/검증 범위: build만 수행 (flash 미실행)
- ui 폴더의 ui 생성파일은 수정하지않음

## 2) 현재 상태 요약
- `next_song` 파싱/선택 로직 개선 반영 완료
- `time_now`, `total_time`, `seekbar` UI 연동 완료
- seekbar 스타일 반영 완료
  - 배경: 반투명 검정
  - 진행바: 앨범아트 기반 유사 색상 자동 적용
- seekbar 터치 기반 시크(탭/드래그 후 release) 명령 반영 완료
  - 명령 엔드포인트: `/api/v1/seek-to`

## 3) 이번까지 완료된 핵심 작업
- clean+build 시작 스크립트 추가
  - `tools/start_clean_build.ps1`
  - `-ResetManagedComponents` 옵션 지원
- 폴링 주기 단축
  - `YTMD_POLL_INTERVAL_MS: 2000 -> 500`
- 앨범아트 프리패치 비동기화 + 요청 coalescing + 캐시 lock
- album task 즉시 깨우기 경로 추가
  - `ulTaskNotifyTake` 기반 대기
  - next/prev 직후 즉시 refresh notify
- 보조 상태(enrich) 주기 분리
  - `YTMD_AUX_STATE_ENRICH_INTERVAL_MS = 1500`
- `next_song` 라벨 경로 개선
  - `/song` 힌트(title/artist/videoId) 저장
  - `/queue` 전수 파싱 후 선택
  - 중복 renderer 제거
  - `selected` 미검출 시 힌트 매칭 복원
  - row 기반 다음 항목 선택
  - 디버그 로그 추가
    - `NEXT: queue-cache resolve selected_pos=...`
    - `NEXT: parsed from /queue => '...'`
- 플레이 타임라인 UI 연동
  - `time_now`/`total_time` 포맷 표시
  - seekbar range/value 동기화
- seekbar 터치 시크
  - touch 좌표 -> seconds 변환
  - `LV_EVENT_PRESSED/PRESSING/RELEASED/PRESS_LOST` 처리
  - hit-area 확장(`lv_obj_set_ext_click_area`)
  - release 시 `ytmd_client_cmd_seek_to(seconds)` 전송

## 4) 빌드/업로드 결과
- build: 성공 (여러 차례)
- app-flash: 이번 세션에서는 미실행

## 5) 주요 파일 변경 (현재 기준)
- `main/ytmd_client.c`
  - next_song 파서/큐 캐시/선택 로직 개선
  - `ytmd_client_cmd_seek_to(int seconds)` 추가 (`/api/v1/seek-to`)
- `main/ytmd_client.h`
  - seek-to API 선언 추가
- `main/player_ui.c`
  - `time_now`/`total_time`/`seekbar` 바인딩 및 갱신
  - seekbar 스타일(반투명 배경 + 유사색 indicator)
  - seekbar 터치 이벤트 기반 시크 동작
- `main/player_ui.h`
  - `ytmd_cmd_seek_to(int seconds)` 선언 추가
- `main/main.c`
  - UI control ops(prev/next) 커스텀 연결 및 상태 반영 유지
- `main/ui/screens.c`, `main/ui/screens.h`
  - seekbar/time_now/total_time 오브젝트 반영
- `tools/start_clean_build.ps1` (신규)

## 6) 다음 세션 즉시 할 일
1. 장비 연결 후 `app-flash` 수행 (필요 시 COM 포트 점유 해제)
2. 실기기에서 아래 항목 확인
   - seekbar 탭/드래그 시 실제 재생 위치 이동
   - `time_now`/`total_time` 표기 일치
   - `next_song` 라벨 실제 다음곡 일치
3. 필요 시 seek-to payload 형식(`{"seconds":N}`) 서버 호환성 재확인

## 7) 자주 쓰는 명령
```bash
# 시작 준비: clean + build
powershell -ExecutionPolicy Bypass -File .\tools\start_clean_build.ps1

# managed_components까지 초기화가 필요할 때
powershell -ExecutionPolicy Bypass -File .\tools\start_clean_build.ps1 -ResetManagedComponents

# 빌드
. C:\esp\v6.0\esp-idf\export.ps1; idf.py -B build_DEV_TEAM_2 build

# 업로드 (필요 시)
. C:\esp\v6.0\esp-idf\export.ps1; idf.py -B build_DEV_TEAM_2 -p COM26 app-flash
```

## 8) 다음 세션 시작 프롬프트 (복붙용)
"AI_HANDOFF.md 기준으로 이어서 진행. 우선 app-flash 후 seekbar 터치 시크(`/api/v1/seek-to`)와 time_now/total_time, next_song 정확도 실기기 검증해줘."

---

## Session Update (2026-04-25)

- UI generated files are managed in EEZ Studio. Do not manually edit `main/ui/*` unless explicitly requested.
- Playlist navigation wiring is done in `main/player_ui.c`:
  - `golist` click -> `SCREEN_ID_PLAYLIST`
  - playlist back icon click (`ui_image_arrow_left`) -> `SCREEN_ID_MAIN`
- `nowplay` label now shows current `title-artist` and uses the same font size as `song_artist`.
- UI object name conflict was resolved in UI side by renaming `return` -> `return_main`.
- Build verification completed:
  - `. C:\esp\v6.0\esp-idf\export.ps1; idf.py -B build_DEV_TEAM_2 build`
  - Result: PASS
- Current checkpoint commit:
  - `d4e86c5` (`feat: wire playlist scene navigation and nowplay label`)
