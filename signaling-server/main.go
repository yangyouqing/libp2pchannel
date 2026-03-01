package main

import (
	"log"
	"net"
	"os"
	"os/signal"
	"syscall"

	"github.com/libp2pchannel/signaling-server/config"
	"github.com/libp2pchannel/signaling-server/handler"
)

func main() {
	cfg := config.LoadFromEnv()

	log.Printf("P2P Signaling Server starting on %s", cfg.ListenAddr)
	log.Printf("TURN server: %s:%d (realm: %s)", cfg.TURNServerHost, cfg.TURNServerPort, cfg.TURNRealm)
	log.Printf("Max subscribers per room: %d", cfg.MaxSubscribers)

	h := handler.NewSignalingHandler(cfg)

	listener, err := net.Listen("tcp", cfg.ListenAddr)
	if err != nil {
		log.Fatalf("Failed to listen on %s: %v", cfg.ListenAddr, err)
	}
	defer listener.Close()

	// Graceful shutdown
	done := make(chan os.Signal, 1)
	signal.Notify(done, syscall.SIGINT, syscall.SIGTERM)

	go func() {
		<-done
		log.Println("Shutting down...")
		listener.Close()
	}()

	log.Printf("Signaling server listening on %s", cfg.ListenAddr)

	for {
		conn, err := listener.Accept()
		if err != nil {
			select {
			case <-done:
				return
			default:
				log.Printf("Accept error: %v", err)
				continue
			}
		}
		go h.HandleConnection(conn)
	}
}
