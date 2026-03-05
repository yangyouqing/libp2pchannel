package handler

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"time"

	"github.com/libp2pchannel/signaling-server/middleware"
	"github.com/libp2pchannel/signaling-server/model"
)

func (h *Handler) HandleSSE(w http.ResponseWriter, r *http.Request) {
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming not supported", http.StatusInternalServerError)
		return
	}

	peerID, _ := r.Context().Value(middleware.PeerIDKey).(string)
	if peerID == "" {
		peerID = r.URL.Query().Get("peer_id")
	}
	if peerID == "" {
		http.Error(w, `{"error":"peer_id required"}`, http.StatusBadRequest)
		return
	}

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("X-Accel-Buffering", "no")
	w.Header().Set("X-Node-ID", h.Config.NodeID)
	w.WriteHeader(http.StatusOK)
	flusher.Flush()

	events := make(chan model.SSEEvent, 64)

	h.Store.RegisterPeer(peerID, h.Config.NodeID)
	h.PubSub.RegisterPeer(peerID, events)
	defer h.PubSub.UnregisterPeer(peerID)

	defer func() {
		peer, _ := h.Store.GetPeer(peerID)
		if peer != nil && peer.RoomID != "" {
			room, _ := h.Store.GetRoom(peer.RoomID)
			if room != nil {
				leftData, _ := json.Marshal(map[string]string{"peer_id": peerID})
				if peer.IsPublisher {
					for _, subID := range room.SubscriberIDs {
						h.publishToPeer(subID, model.SSEEvent{
							Type: "peer_left",
							Data: string(leftData),
						})
					}
				} else if room.PublisherID != "" {
					h.publishToPeer(room.PublisherID, model.SSEEvent{
						Type: "peer_left",
						Data: string(leftData),
					})
				}
			}
		}
		h.Store.UnregisterPeer(peerID)
	}()

	log.Printf("[sse] peer %s connected (node %s)", peerID, h.Config.NodeID)

	pingInterval := time.Duration(h.Config.SSEPingInterval) * time.Second
	if pingInterval <= 0 {
		pingInterval = 15 * time.Second
	}
	ticker := time.NewTicker(pingInterval)
	defer ticker.Stop()

	for {
		select {
		case evt := <-events:
			if _, err := fmt.Fprintf(w, "event: %s\ndata: %s\n\n", evt.Type, evt.Data); err != nil {
				log.Printf("[sse] write error for peer %s: %v", peerID, err)
				return
			}
			flusher.Flush()

		case <-ticker.C:
			if _, err := fmt.Fprintf(w, "event: ping\ndata: {}\n\n"); err != nil {
				log.Printf("[sse] ping write error for peer %s: %v", peerID, err)
				return
			}
			flusher.Flush()

		case <-r.Context().Done():
			log.Printf("[sse] peer %s disconnected", peerID)
			return
		}
	}
}
