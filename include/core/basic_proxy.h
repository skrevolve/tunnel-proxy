#pragma once

#include <string>
#include <atomic>
#include <cstdint>

/**
 * 기본 TCP 프록시 구현
 * 
 * 멀티스레드 기반의 단순 TCP 터널 프록시입니다.
 * 클라이언트 연결을 받아 타겟 서버로 전달하고,
 * 양방향 데이터 포워딩을 수행합니다.
 */
class BasicProxy {
public:
    /**
     * 프록시 생성자
     * @param local_port 로컬 바인딩 포트
     * @param target_ip 타겟 서버 IP
     * @param target_port 타겟 서버 포트
     */
    BasicProxy(int local_port, const std::string& target_ip, int target_port);
    
    /**
     * 프록시 소멸자
     */
    ~BasicProxy();
    
    /**
     * 프록시 시작 (메인 루프)
     * 클라이언트 연결을 받아 처리합니다.
     */
    void run();
    
    /**
     * 프록시 중지
     */
    void stop();
    
    /**
     * 총 연결 수 조회
     */
    uint64_t get_total_connections() const;
    
    /**
     * 활성 연결 수 조회
     */
    uint64_t get_active_connections() const;

private:
    /**
     * 로컬 리스닝 소켓 생성
     * @param port 바인딩할 포트
     * @return 소켓 파일 디스크립터 (실패시 -1)
     */
    int create_listening_socket(int port);
    
    /**
     * 타겟 서버에 연결
     * @param ip 타겟 서버 IP
     * @param port 타겟 서버 포트
     * @return 소켓 파일 디스크립터 (실패시 -1)
     */
    int connect_to_target(const std::string& ip, int port);
    
    /**
     * 클라이언트 연결 처리
     * @param client_fd 클라이언트 소켓 파일 디스크립터
     */
    void handle_connection(int client_fd);
    
    /**
     * 두 소켓 간 데이터 포워딩
     * @param from_fd 소스 소켓 파일 디스크립터
     * @param to_fd 목표 소켓 파일 디스크립터
     */
    void forward_data(int from_fd, int to_fd);
    
    // 멤버 변수
    int listen_fd_;                          // 로컬 리스닝 소켓
    std::string target_ip_;                  // 타겟 서버 IP
    int target_port_;                        // 타겟 서버 포트
    bool running_;                           // 실행 상태 플래그
    std::atomic<uint64_t> total_connections_; // 누적 연결 수
    std::atomic<uint64_t> active_connections_; // 현재 활성 연결 수
};
