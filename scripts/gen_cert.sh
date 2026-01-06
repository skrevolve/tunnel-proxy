#!/bin/bash
# TLS 인증서 생성 스크립트 (Phase 4용)

echo "Generating self-signed certificate..."

openssl req -x509 -newkey rsa:4096 \
    -keyout ../config/server.key \
    -out ../config/server.crt \
    -days 365 -nodes \
    -subj "/C=KR/ST=Seoul/L=Seoul/O=Proxy/CN=localhost"

echo "Certificate generated:"
echo "  - config/server.crt"
echo "  - config/server.key"
