#include "utils/config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

Config Config::load_from_file(const std::string& path) {
    // ── 파일 열기 ──────────────────────────────────────────────────────────────
    //
    // ifstream의 기본 모드는 읽기(ios::in)이다.
    // is_open() 확인 이유: 파일이 없거나 권한이 없으면 예외가 아닌 실패 상태가 되므로
    // 명시적으로 확인해야 한다.
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open config file: " + path);
    }

    // ── JSON 파싱 ──────────────────────────────────────────────────────────────
    //
    // nlohmann::json은 >> 연산자로 스트림에서 직접 파싱할 수 있다.
    // 형식이 잘못됐을 때 parse_error 예외를 던지므로 try-catch로 감싼다.
    // parse_error는 어느 위치에서 오류가 났는지 e.what()에 포함한다.
    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error("JSON parse error in " + path + ": " + e.what());
    }

    // ── 필드 추출 ──────────────────────────────────────────────────────────────
    //
    // j.at("key")   : 키가 없으면 out_of_range 예외 발생 → 필수 필드에 사용
    // j.value("key", default) : 키가 없으면 기본값 반환   → 선택 필드에 사용
    //
    // get<T>() : JSON 값을 C++ 타입으로 변환.
    //            타입이 맞지 않으면 type_error 예외 발생.
    //            예) "local_port": "8080" (문자열)이면 get<int>() 실패.
    Config config;
    config.local_port_  = j.at("local_port").get<int>();
    config.target_ip_   = j.at("target_ip").get<std::string>();
    config.target_port_ = j.at("target_port").get<int>();

    // 선택 필드: 없으면 기본값 사용
    config.mode_     = j.value("mode",     "tcp");   // Phase 5까지는 tcp만 사용
    config.verbose_  = j.value("verbose",  false);   // false면 INFO 이상만 출력
    config.log_file_ = j.value("log_file", "");      // 빈 문자열이면 파일 출력 안 함

    return config;
    // 반환 시 config 객체가 복사(또는 NRVO 최적화로 이동)된다.
    // 컴파일러는 대부분 NRVO(Named Return Value Optimization)를 적용해
    // 복사 없이 호출자의 메모리에 직접 생성한다.
}
