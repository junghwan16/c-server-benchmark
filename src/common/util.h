#pragma once

/**
 * @brief 파일 디스크립터를 논블로킹 모드로 설정
 * @param fd 파일 디스크립터
 * @return 성공 시 0, 실패 시 -1 반환
 */
int set_nonblock(int fd);