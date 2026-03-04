package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/libp2pchannel/signaling-server/auth"
	"github.com/libp2pchannel/signaling-server/config"
	"github.com/libp2pchannel/signaling-server/handler"
	"github.com/libp2pchannel/signaling-server/middleware"
	"github.com/libp2pchannel/signaling-server/pubsub"
	"github.com/libp2pchannel/signaling-server/store"
)

func main() {
	cfg := config.LoadFromEnv()

	log.Printf("P2P Signaling Server starting on %s (node %s)", cfg.ListenAddr, cfg.NodeID)
	log.Printf("TLS cert: %s, key: %s", cfg.TLSCertFile, cfg.TLSKeyFile)
	for i, ts := range cfg.TURNServers {
		log.Printf("TURN server #%d: %s:%d", i+1, ts.Host, ts.Port)
	}
	log.Printf("Max subscribers per room: %d", cfg.MaxSubscribers)

	var (
		st store.Store
		ps pubsub.PubSub
	)
	mode := "memory"

	if cfg.RedisURL != "" {
		mode = "redis"
		log.Printf("Redis mode enabled: %s", cfg.RedisURL)
		rs, err := store.NewRedisStore(cfg.RedisURL, cfg.NodeID, cfg.MaxSubscribers)
		if err != nil {
			log.Fatalf("Failed to connect to Redis: %v", err)
		}
		st = rs
		rps, err := pubsub.NewRedisPubSub(cfg.RedisURL, cfg.NodeID)
		if err != nil {
			log.Fatalf("Failed to create Redis PubSub: %v", err)
		}
		ps = rps
	} else {
		log.Printf("Memory mode (no REDIS_URL configured)")
		st = store.NewMemoryStore()
		ps = pubsub.NewLocalPubSub()
	}

	h := handler.NewHandler(cfg, st, ps)
	authMW := middleware.Auth(cfg.JWTSecret, cfg.AdminSecret)

	mux := http.NewServeMux()
	mux.HandleFunc("/v1/signal", h.HandleSignal)
	mux.HandleFunc("/v1/events", h.HandleSSE)

	mux.HandleFunc("/v1/admin/peers/", h.HandleAdminPeerByID)
	mux.HandleFunc("/v1/admin/peers", h.HandleAdminPeers)
	mux.HandleFunc("/v1/admin/rooms/", h.HandleAdminRoomByID)
	mux.HandleFunc("/v1/admin/rooms", h.HandleAdminRooms)

	mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		status := map[string]interface{}{
			"status":  "ok",
			"node_id": cfg.NodeID,
			"mode":    mode,
		}
		if cfg.RedisURL != "" {
			if rs, ok := st.(*store.RedisStore); ok {
				if err := rs.Ping(r.Context()); err != nil {
					status["status"] = "degraded"
					status["redis"] = "error: " + err.Error()
				} else {
					status["redis"] = "ok"
				}
			}
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(status)
	})

	mux.HandleFunc("/v1/token", func(w http.ResponseWriter, r *http.Request) {
		authHeader := r.Header.Get("Authorization")
		if !strings.HasPrefix(authHeader, "Bearer ") ||
			strings.TrimPrefix(authHeader, "Bearer ") != cfg.AdminSecret {
			http.Error(w, `{"error":"admin auth required"}`, http.StatusUnauthorized)
			return
		}
		peerID := r.URL.Query().Get("peer_id")
		if peerID == "" {
			http.Error(w, `{"error":"peer_id query param required"}`, http.StatusBadRequest)
			return
		}
		token, err := auth.GenerateToken(peerID, cfg.JWTSecret, 24*time.Hour)
		if err != nil {
			http.Error(w, `{"error":"token generation failed"}`, http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprintf(w, `{"token":"%s"}`, token)
	})

	wrapped := authMW(mux)

	srv := &http.Server{
		Addr:         cfg.ListenAddr,
		Handler:      wrapped,
		ReadTimeout:  30 * time.Second,
		WriteTimeout: 0,
		IdleTimeout:  120 * time.Second,
	}

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	go func() {
		<-sigCh
		log.Println("Shutting down...")
		ps.Close()
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		srv.Shutdown(ctx)
	}()

	log.Printf("Signaling server listening on %s (HTTPS, mode=%s)", cfg.ListenAddr, mode)

	if err := srv.ListenAndServeTLS(cfg.TLSCertFile, cfg.TLSKeyFile); err != nil && err != http.ErrServerClosed {
		log.Fatalf("ListenAndServeTLS: %v", err)
	}
}
