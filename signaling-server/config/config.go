package config

import (
	"crypto/rand"
	"encoding/json"
	"fmt"
	"os"
	"strconv"
)

type TURNServerConfig struct {
	Host   string `json:"host"`
	Port   uint16 `json:"port"`
	Secret string `json:"secret,omitempty"`
}

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
	TURNCredTTL      uint32
	TURNServers      []TURNServerConfig
	MaxSubscribers   int
	SSEPingInterval  int
	RedisURL         string
	NodeID           string
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

func generateNodeID() string {
	b := make([]byte, 8)
	rand.Read(b)
	return fmt.Sprintf("node-%x", b)
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
	if v := os.Getenv("REDIS_URL"); v != "" {
		cfg.RedisURL = v
	}
	if v := os.Getenv("NODE_ID"); v != "" {
		cfg.NodeID = v
	} else {
		cfg.NodeID = generateNodeID()
	}
	if v := os.Getenv("TURN_SERVERS"); v != "" {
		var servers []TURNServerConfig
		if err := json.Unmarshal([]byte(v), &servers); err == nil {
			cfg.TURNServers = servers
		}
	}

	if len(cfg.TURNServers) == 0 {
		cfg.TURNServers = []TURNServerConfig{
			{Host: cfg.TURNServerHost, Port: cfg.TURNServerPort, Secret: cfg.TURNSharedSecret},
		}
	}

	return cfg
}
