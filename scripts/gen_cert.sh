#!/bin/bash
# gen_cert.sh — 개발/테스트용 자체 서명 인증서 생성
#
# 출력:
#   certs/server.crt  — 서버 인증서 (공개)
#   certs/server.key  — 서버 개인키 (비밀)
#
# 주의: 자체 서명 인증서는 신뢰된 CA가 서명하지 않으므로
#       실제 서비스에서는 Let's Encrypt 등 공인 CA 인증서를 사용해야 한다.

set -e

CERT_DIR="$(dirname "$0")/../certs"
mkdir -p "$CERT_DIR"

CERT="$CERT_DIR/server.crt"
KEY="$CERT_DIR/server.key"

# RSA 2048비트 개인키 + 자체 서명 인증서 (유효기간 365일)
# -nodes: 패스프레이즈 없이 저장 (테스트 편의용)
openssl req \
    -x509 \
    -newkey rsa:2048 \
    -keyout "$KEY" \
    -out "$CERT" \
    -days 365 \
    -nodes \
    -subj "/C=KR/ST=Seoul/O=TunnelProxy/CN=localhost"

echo "생성 완료:"
echo "  인증서: $CERT"
echo "  개인키: $KEY"
openssl x509 -in "$CERT" -noout -dates
