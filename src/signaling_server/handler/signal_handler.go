package handler

import (
	"context"
	"encoding/json"
	"log"
	"net/http"

	"github.com/libp2pchannel/signaling-server/auth"
	"github.com/libp2pchannel/signaling-server/config"
	"github.com/libp2pchannel/signaling-server/model"
	"github.com/libp2pchannel/signaling-server/pubsub"
	"github.com/libp2pchannel/signaling-server/store"
	"github.com/libp2pchannel/signaling-server/turn"
)

type Handler struct {
	Store  store.Store
	PubSub pubsub.PubSub
	Config *config.Config
}

func NewHandler(cfg *config.Config, s store.Store, ps pubsub.PubSub) *Handler {
	return &Handler{
		Store:  s,
		PubSub: ps,
		Config: cfg,
	}
}

func writeJSON(w http.ResponseWriter, status int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(v)
}

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, model.SignalResponse{Type: "error", Error: msg})
}

func (h *Handler) HandleSignal(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeError(w, http.StatusMethodNotAllowed, "POST required")
		return
	}

	var req model.SignalRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}

	if req.Token != "" {
		peerID, err := auth.ValidateToken(req.Token, h.Config.JWTSecret)
		if err != nil {
			writeError(w, http.StatusForbidden, "invalid token")
			return
		}
		if req.PeerID == "" {
			req.PeerID = peerID
		}
	}

	if req.PeerID == "" {
		writeError(w, http.StatusBadRequest, "peer_id required")
		return
	}

	switch req.Type {
	case "create_room":
		h.handleCreateRoom(w, &req)
	case "join_room":
		h.handleJoinRoom(w, &req)
	case "leave_room":
		h.handleLeaveRoom(w, &req)
	case "ice_offer":
		h.handleICEForward(w, &req, "ice_offer")
	case "ice_answer":
		h.handleICEForward(w, &req, "ice_answer")
	case "ice_candidate":
		h.handleICECandidate(w, &req)
	case "gathering_done":
		h.handleICEForward(w, &req, "gathering_done")
	case "full_offer":
		h.handleFullOffer(w, &req)
	case "full_answer":
		h.handleFullAnswer(w, &req)
	case "request_offer":
		h.handleICEForward(w, &req, "request_offer")
	default:
		writeError(w, http.StatusBadRequest, "unknown type: "+req.Type)
	}
}

func (h *Handler) handleCreateRoom(w http.ResponseWriter, req *model.SignalRequest) {
	roomID := req.RoomID
	if roomID == "" {
		writeError(w, http.StatusBadRequest, "room_id required")
		return
	}

	h.Store.EnsurePeer(req.PeerID, h.Config.NodeID)
	if err := h.Store.CreateRoom(roomID, req.PeerID, h.Config.MaxSubscribers); err != nil {
		writeError(w, http.StatusConflict, err.Error())
		return
	}

	writeJSON(w, http.StatusOK, model.SignalResponse{
		Type:   "room_created",
		RoomID: roomID,
	})
}

func (h *Handler) handleJoinRoom(w http.ResponseWriter, req *model.SignalRequest) {
	roomID := req.RoomID
	if roomID == "" {
		writeError(w, http.StatusBadRequest, "room_id required")
		return
	}

	h.Store.EnsurePeer(req.PeerID, h.Config.NodeID)
	roomMeta, err := h.Store.JoinRoom(roomID, req.PeerID)
	if err != nil {
		writeError(w, http.StatusConflict, err.Error())
		return
	}

	turnInfos := h.generateTURNCredentials(req.PeerID)

	// Notify publisher via pub/sub and also send TURN credentials
	if roomMeta.PublisherID != "" {
		pubTurns := h.generateTURNCredentials(roomMeta.PublisherID)
		turnData, _ := json.Marshal(pubTurns[0])
		h.publishToPeer(roomMeta.PublisherID, model.SSEEvent{
			Type: "turn_credentials",
			Data: string(turnData),
		})

		joinedData, _ := json.Marshal(map[string]string{"peer_id": req.PeerID})
		h.publishToPeer(roomMeta.PublisherID, model.SSEEvent{
			Type: "peer_joined",
			Data: string(joinedData),
		})

		// Notify subscriber of publisher so it can request_offer as fallback
		pubReadyData, _ := json.Marshal(map[string]string{"publisher_id": roomMeta.PublisherID})
		h.publishToPeer(req.PeerID, model.SSEEvent{
			Type: "publisher_ready",
			Data: string(pubReadyData),
		})
	}

	resp := model.SignalResponse{
		Type:        "joined",
		RoomID:      roomID,
		Turn:        &turnInfos[0],
		TurnServers: turnInfos,
	}

	writeJSON(w, http.StatusOK, resp)
}

func (h *Handler) handleLeaveRoom(w http.ResponseWriter, req *model.SignalRequest) {
	peer, _ := h.Store.GetPeer(req.PeerID)
	if peer != nil && peer.RoomID != "" {
		room, _ := h.Store.GetRoom(peer.RoomID)
		if room != nil {
			leftData, _ := json.Marshal(map[string]string{"peer_id": req.PeerID})
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
	h.Store.LeaveRoom(req.PeerID)
	writeJSON(w, http.StatusOK, model.SignalResponse{Type: "ok"})
}

func (h *Handler) handleICEForward(w http.ResponseWriter, req *model.SignalRequest, eventType string) {
	if req.To == "" {
		writeError(w, http.StatusBadRequest, "to required")
		return
	}

	payload := map[string]interface{}{"from": req.PeerID}
	if req.SDP != "" {
		payload["sdp"] = req.SDP
	}

	data, _ := json.Marshal(payload)
	if err := h.publishToPeer(req.To, model.SSEEvent{Type: eventType, Data: string(data)}); err != nil {
		log.Printf("[handler] forward %s from %s to %s failed: %v",
			eventType, req.PeerID, req.To, err)
		writeError(w, http.StatusNotFound, err.Error())
		return
	}

	writeJSON(w, http.StatusOK, model.SignalResponse{Type: "ok"})
}

func (h *Handler) handleICECandidate(w http.ResponseWriter, req *model.SignalRequest) {
	if req.To == "" {
		writeError(w, http.StatusBadRequest, "to required")
		return
	}

	payload := map[string]string{
		"from":      req.PeerID,
		"candidate": req.Candidate,
	}
	data, _ := json.Marshal(payload)
	if err := h.publishToPeer(req.To, model.SSEEvent{Type: "ice_candidate", Data: string(data)}); err != nil {
		writeError(w, http.StatusNotFound, err.Error())
		return
	}

	writeJSON(w, http.StatusOK, model.SignalResponse{Type: "ok"})
}

func (h *Handler) handleFullOffer(w http.ResponseWriter, req *model.SignalRequest) {
	if req.To == "" {
		writeError(w, http.StatusBadRequest, "to required")
		return
	}

	offer := &store.CachedICE{
		From:       req.PeerID,
		SDP:        req.SDP,
		Candidates: req.Candidates,
	}

	if req.RoomID != "" {
		h.Store.CacheOffer(req.RoomID, offer)
	}

	data, _ := json.Marshal(offer)
	if err := h.publishToPeer(req.To, model.SSEEvent{Type: "full_offer", Data: string(data)}); err != nil {
		log.Printf("[handler] forward full_offer from %s to %s failed: %v",
			req.PeerID, req.To, err)
		writeError(w, http.StatusNotFound, err.Error())
		return
	}

	writeJSON(w, http.StatusOK, model.SignalResponse{Type: "ok"})
}

func (h *Handler) handleFullAnswer(w http.ResponseWriter, req *model.SignalRequest) {
	if req.To == "" {
		writeError(w, http.StatusBadRequest, "to required")
		return
	}

	answer := map[string]interface{}{
		"from":       req.PeerID,
		"sdp":        req.SDP,
		"candidates": req.Candidates,
	}
	data, _ := json.Marshal(answer)
	if err := h.publishToPeer(req.To, model.SSEEvent{Type: "full_answer", Data: string(data)}); err != nil {
		log.Printf("[handler] forward full_answer from %s to %s failed: %v",
			req.PeerID, req.To, err)
		writeError(w, http.StatusNotFound, err.Error())
		return
	}

	writeJSON(w, http.StatusOK, model.SignalResponse{Type: "ok"})
}

func (h *Handler) generateTURNCredentials(peerID string) []model.TurnInfo {
	infos := make([]model.TurnInfo, 0, len(h.Config.TURNServers))
	for _, ts := range h.Config.TURNServers {
		secret := ts.Secret
		if secret == "" {
			secret = h.Config.TURNSharedSecret
		}
		username, password := turn.GenerateCredentials(secret, peerID, h.Config.TURNCredTTL)
		infos = append(infos, model.TurnInfo{
			Username: username,
			Password: password,
			Server:   ts.Host,
			Port:     ts.Port,
			TTL:      h.Config.TURNCredTTL,
		})
	}
	return infos
}

func (h *Handler) publishToPeer(peerID string, evt model.SSEEvent) error {
	peer, _ := h.Store.GetPeer(peerID)
	nodeID := ""
	if peer != nil {
		nodeID = peer.NodeID
	}
	return h.PubSub.Publish(context.Background(), pubsub.Event{
		TargetPeerID: peerID,
		TargetNodeID: nodeID,
		SSEEvent:     evt,
	})
}
