#!/bin/bash
# gen_cert.sh — 개발/테스트용 인증서 생성
#
# 일반 TLS용 (Phase 4, 기본 모드):
#   certs/server.crt / certs/server.key
#
# mTLS용 (Phase 7): --mtls 옵션 사용
#   certs/ca.crt / certs/ca.key         — 자체 서명 CA
#   certs/server.crt / certs/server.key — CA가 서명한 서버 인증서
#   certs/client.crt / certs/client.key — CA가 서명한 클라이언트 인증서
#
# 주의: 개발/테스트 전용. 운영 환경에서는 공인 CA 인증서를 사용하라.

set -e

CERT_DIR="$(dirname "$0")/../certs"
mkdir -p "$CERT_DIR"

MODE="${1:-}"

# ── mTLS 모드 ─────────────────────────────────────────────────────────────────
if [ "$MODE" = "--mtls" ]; then
    CA_KEY="$CERT_DIR/ca.key"
    CA_CERT="$CERT_DIR/ca.crt"
    SERVER_KEY="$CERT_DIR/server.key"
    SERVER_CSR="$CERT_DIR/server.csr"
    SERVER_CERT="$CERT_DIR/server.crt"
    CLIENT_KEY="$CERT_DIR/client.key"
    CLIENT_CSR="$CERT_DIR/client.csr"
    CLIENT_CERT="$CERT_DIR/client.crt"

    echo "=== mTLS 인증서 생성 ==="

    # 1. CA 개인키 + 자체 서명 CA 인증서 (유효기간 3650일 = 10년)
    echo "[1/3] CA 인증서 생성..."
    openssl genrsa -out "$CA_KEY" 2048 2>/dev/null
    openssl req -x509 -new -nodes \
        -key "$CA_KEY" \
        -days 3650 \
        -out "$CA_CERT" \
        -subj "/C=KR/ST=Seoul/O=TunnelProxy/CN=TunnelProxy-CA"

    # 2. 서버 인증서: CSR 생성 → CA 서명
    echo "[2/3] 서버 인증서 생성 (CA 서명)..."
    openssl genrsa -out "$SERVER_KEY" 2048 2>/dev/null
    openssl req -new \
        -key "$SERVER_KEY" \
        -out "$SERVER_CSR" \
        -subj "/C=KR/ST=Seoul/O=TunnelProxy/CN=tunnel-server"
    openssl x509 -req \
        -in "$SERVER_CSR" \
        -CA "$CA_CERT" -CAkey "$CA_KEY" -CAcreateserial \
        -days 365 \
        -out "$SERVER_CERT"
    rm -f "$SERVER_CSR"

    # 3. 클라이언트 인증서: CSR 생성 → CA 서명
    echo "[3/3] 클라이언트 인증서 생성 (CA 서명)..."
    openssl genrsa -out "$CLIENT_KEY" 2048 2>/dev/null
    openssl req -new \
        -key "$CLIENT_KEY" \
        -out "$CLIENT_CSR" \
        -subj "/C=KR/ST=Seoul/O=TunnelProxy/CN=tunnel-agent-001"
    openssl x509 -req \
        -in "$CLIENT_CSR" \
        -CA "$CA_CERT" -CAkey "$CA_KEY" -CAcreateserial \
        -days 365 \
        -out "$CLIENT_CERT"
    rm -f "$CLIENT_CSR"

    echo ""
    echo "=== 생성 완료 ==="
    echo "  CA 인증서:         $CA_CERT"
    echo "  서버 인증서:       $SERVER_CERT"
    echo "  클라이언트 인증서: $CLIENT_CERT"
    echo ""
    echo "=== 유효 기간 ==="
    openssl x509 -in "$SERVER_CERT"  -noout -dates | sed 's/^/  서버       /'
    openssl x509 -in "$CLIENT_CERT" -noout -dates | sed 's/^/  클라이언트 /'
    echo ""
    echo "=== CA 서명 검증 ==="
    openssl verify -CAfile "$CA_CERT" "$SERVER_CERT"  && echo "  서버 인증서: OK"
    openssl verify -CAfile "$CA_CERT" "$CLIENT_CERT" && echo "  클라이언트 인증서: OK"
    exit 0
fi

# ── 기본 모드: 일반 TLS 자체 서명 인증서 (Phase 4) ───────────────────────────
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
