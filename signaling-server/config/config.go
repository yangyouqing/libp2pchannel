package config

import (
	"os"
	"strconv"
)

type Config struct {
	ListenAddr       string
	TURNServerHost   string
	TURNServerPort   uint16
	TURNSharedSecret string
	TURNRealm        string
	TURNCredTTL      uint32 // seconds
	MaxSubscribers   int
	HeartbeatTimeout int // seconds
}

func DefaultConfig() *Config {
	return &Config{
		ListenAddr:       ":8080",
		TURNServerHost:   "127.0.0.1",
		TURNServerPort:   3478,
		TURNSharedSecret: "p2p-turn-secret",
		TURNRealm:        "p2p-av",
		TURNCredTTL:      86400,
		MaxSubscribers:   5,
		HeartbeatTimeout: 30,
	}
}

func LoadFromEnv() *Config {
	cfg := DefaultConfig()
	if v := os.Getenv("LISTEN_ADDR"); v != "" {
		cfg.ListenAddr = v
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
	return cfg
}
