# AI Handoff

목적: 다른 PC/대화창에서도 지금 상태를 그대로 이어서 작업하기 위한 인수인계 문서

## 1) 세션 메타
- 프로젝트: ESP32P4_RNDIS_or_NCM
- 최종 업데이트(KST): 2026-04-24 02:39
- 브랜치: YTMD
- HEAD: e056f64 (`이미지리소스정리_플레이어UI개선`)
- 타겟/SDK: esp32p4 / ESP-IDF v6.0
- 워킹트리 상태: clean (미커밋 변경 없음)

## 2) 현재 목표
- 목표: YTMD 플레이어 UI 반응성/표시 안정화 (재생상태, next/prev 피드백, 앨범아트 전환)
- 현재 이슈: 곡 전환 체감이 약 2초 (poll 주기 영향)

## 3) 이번까지 완료된 핵심 작업
- `isPaused` 반영으로 중앙 play/pause 아이콘 상태 동기화
- 부팅 직후에도 playback 상태가 UI에 반영되도록 로직 수정
- next/prev 클릭 시 push 이미지 적용 + 곡 변경/seek 조건 충족 시 원복
- next song 파싱/표시 안정화 (`/api/v1/queue` 기반)
- repeat/shuffle/like/dislike 엔드포인트 매핑 정리
- 앨범아트 모서리 라운드 처리 안정화
- 전체 배경에 앨범아트 dim 레이어 추가 (어둡게/흐린 느낌)
- `Playback state updated` 과다 로그 주석 처리
- 앨범아트 5장 캐시 추가 (PSRAM 우선)
- queue 기준 `이전2-이전-현재-다음-다다음` 프리패치 구현
- prefetch에서 발생한 stack protection fault 수정 (대형 로컬 버퍼 -> heap 할당)
- prefetch JPEG 경로 zoom 규칙 보정 (비정사각 썸네일이 작아 보이는 문제 완화)

## 4) 성능/동작 메모
- 체감 2초 지연의 주원인: `YTMD_POLL_INTERVAL_MS = 2000`
- 현재는 poll 기반이라, 상태 반영 최소 단위가 2초에 가까움
- 더 줄이려면 다음 중 하나 필요
- 옵션 A: poll 주기 축소 (예: 500~1000ms, 네트워크 요청 증가)
- 옵션 B: 명령 직후 optimistic UI + 백그라운드 정합
- 옵션 C: 가능하면 push/event 기반 API 사용

## 5) 앨범아트 저장 구조(현재)
- 파일 저장 아님, RAM/PSRAM 캐시
- 표시 버퍼: `s_album_frame` (400x400 RGB565)
- 추가 캐시: 5-slot LRU (`YTMD_ART_CACHE_CAPACITY 5`)
- 프리패치 범위: selected 기준 -2..+2

## 6) 오늘 확인한 장애와 조치
- 증상: Guru Meditation / Stack protection fault in `prefetch_art_window_from_queue`
- 원인: 함수 내부 대형 로컬 배열(`targets`)로 task stack 초과
- 조치: heap/PSRAM 동적할당으로 전환 + 모든 조기 리턴 경로 free 처리
- 상태: 빌드 통과, 재발 방지 패치 반영

## 7) 내일 바로 할 일 (우선순위)
- 1) 실기기에서 곡 넘김 시 체감 지연 측정 (현재 약 2초)
- 2) poll 주기 조정 실험 (`2000 -> 1000 -> 500`) 및 네트워크/CPU 영향 비교
- 3) next/prev push 이미지 원복 타이밍 체감 확인
- 4) 배경 앨범아트 dim 강도(현재값) 미세조정
- 5) 필요 시 캐시 hit/miss 로그를 짧게 추가해 실제 prefetch 효율 측정

## 8) 주요 파일
- `main/main.c`
- `main/player_ui.c`
- `main/ytmd_client.c`
- `main/ytmd_client.h`
- `main/ui_display.c`
- `main/ui/screens.c`
- `main/CMakeLists.txt`
- `sdkconfig`

## 9) 자주 쓰는 명령
```bash
# build
idf.py build

# app만 flash
idf.py -p <PORT> app-flash

# monitor
idf.py -p <PORT> monitor

# app + monitor
idf.py -p <PORT> flash monitor
```

## 10) 다음 세션 시작 프롬프트 (복붙용)
"AI_HANDOFF.md 기준으로 이어서 진행. 우선 poll 2초 지연을 줄이는 실험(1000ms/500ms)부터 하고, next/prev push 이미지 전환 체감과 앨범아트 prefetch hit율을 확인해줘."
