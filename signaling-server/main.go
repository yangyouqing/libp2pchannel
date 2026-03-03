package main

import (
	"context"
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
)

func main() {
	cfg := config.LoadFromEnv()

	log.Printf("P2P Signaling Server (HTTPS) starting on %s", cfg.ListenAddr)
	log.Printf("TLS cert: %s, key: %s", cfg.TLSCertFile, cfg.TLSKeyFile)
	log.Printf("TURN server: %s:%d", cfg.TURNServerHost, cfg.TURNServerPort)
	log.Printf("Max subscribers per room: %d", cfg.MaxSubscribers)

	h := handler.NewHandler(cfg)
	authMW := middleware.Auth(cfg.JWTSecret, cfg.AdminSecret)

	mux := http.NewServeMux()
	mux.HandleFunc("/v1/signal", h.HandleSignal)
	mux.HandleFunc("/v1/events", h.HandleSSE)

	// Admin endpoints use prefix matching for path parameters
	mux.HandleFunc("/v1/admin/peers/", h.HandleAdminPeerByID)
	mux.HandleFunc("/v1/admin/peers", h.HandleAdminPeers)
	mux.HandleFunc("/v1/admin/rooms/", h.HandleAdminRoomByID)
	mux.HandleFunc("/v1/admin/rooms", h.HandleAdminRooms)

	// Health check (no auth)
	mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"status":"ok"}`))
	})

	// Token generation endpoint (development/testing, admin-auth protected)
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
		w.Write([]byte(`{"token":"` + token + `"}`))
	})

	wrapped := authMW(mux)

	srv := &http.Server{
		Addr:         cfg.ListenAddr,
		Handler:      wrapped,
		ReadTimeout:  30 * time.Second,
		WriteTimeout: 0, // SSE needs unlimited write timeout
		IdleTimeout:  120 * time.Second,
	}

	// Graceful shutdown
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	go func() {
		<-sigCh
		log.Println("Shutting down...")
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		srv.Shutdown(ctx)
	}()

	log.Printf("Signaling server listening on %s (HTTPS)", cfg.ListenAddr)

	if err := srv.ListenAndServeTLS(cfg.TLSCertFile, cfg.TLSKeyFile); err != nil && err != http.ErrServerClosed {
		log.Fatalf("ListenAndServeTLS: %v", err)
	}
}
