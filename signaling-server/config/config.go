package config

import (
	"os"
	"strconv"
)

type Config struct {
	ListenAddr       string
	TLSCertFile      string
	TLSKeyFile       string
	JWTSecret        string
	AdminSecret      string
	TURNServerHost   string
	TURNServerPort   uint16
	TURNSharedSecret string
	TURNRealm        string
	TURNCredTTL      uint32 // seconds
	MaxSubscribers   int
	SSEPingInterval  int // seconds
}

func DefaultConfig() *Config {
	return &Config{
		ListenAddr:       ":8443",
		TLSCertFile:      "cert.pem",
		TLSKeyFile:       "key.pem",
		JWTSecret:        "p2p-jwt-secret",
		AdminSecret:      "p2p-admin-secret",
		TURNServerHost:   "127.0.0.1",
		TURNServerPort:   3478,
		TURNSharedSecret: "p2p-turn-secret",
		TURNRealm:        "p2p-av",
		TURNCredTTL:      86400,
		MaxSubscribers:   10,
		SSEPingInterval:  15,
	}
}

func LoadFromEnv() *Config {
	cfg := DefaultConfig()
	if v := os.Getenv("LISTEN_ADDR"); v != "" {
		cfg.ListenAddr = v
	}
	if v := os.Getenv("TLS_CERT_FILE"); v != "" {
		cfg.TLSCertFile = v
	}
	if v := os.Getenv("TLS_KEY_FILE"); v != "" {
		cfg.TLSKeyFile = v
	}
	if v := os.Getenv("JWT_SECRET"); v != "" {
		cfg.JWTSecret = v
	}
	if v := os.Getenv("ADMIN_SECRET"); v != "" {
		cfg.AdminSecret = v
	}
	if v := os.Getenv("TURN_HOST"); v != "" {
		cfg.TURNServerHost = v
	}
	if v := os.Getenv("TURN_PORT"); v != "" {
		if p, err := strconv.Atoi(v); err == nil {
			cfg.TURNServerPort = uint16(p)
		}
	}
	if v := os.Getenv("TURN_SECRET"); v != "" {
		cfg.TURNSharedSecret = v
	}
	if v := os.Getenv("TURN_REALM"); v != "" {
		cfg.TURNRealm = v
	}
	if v := os.Getenv("MAX_SUBSCRIBERS"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			cfg.MaxSubscribers = n
		}
	}
	if v := os.Getenv("SSE_PING_INTERVAL"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			cfg.SSEPingInterval = n
		}
	}
	return cfg
}
