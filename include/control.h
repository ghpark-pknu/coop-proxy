/*
 * control.h — 관제 요청 처리
 *
 * 프록시 자신에게 온 요청 (요청 대상이 '/'로 시작: 예 "GET /__stats HTTP/1.1")
 *   /__stats      이 노드의 통계 (JSON)
 *   /__dashboard  클러스터 관제 화면 (HTML, web/dashboard.html)
 *   /             /__dashboard로 이동 (302)
 *   /favicon.ico  내용 없음 (204): 브라우저가 자동으로 요청하는 아이콘
 *   그 밖의 경로   404
 * 관제 요청은 통계와 요청 기록에 넣지 않는다 (대시보드가 1초마다 조회하므로).
 */
#ifndef CONTROL_H
#define CONTROL_H

struct http_request;

/* 관제 요청에 응답하고, 보낸 상태 코드를 반환한다 */
int control_handle(int cfd, const struct http_request *req);

#endif /* CONTROL_H */
