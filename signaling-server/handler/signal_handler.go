package handler

import (
	"encoding/json"
	"log"
	"net/http"

	"github.com/libp2pchannel/signaling-server/auth"
	"github.com/libp2pchannel/signaling-server/config"
	"github.com/libp2pchannel/signaling-server/model"
	"github.com/libp2pchannel/signaling-server/room"
	"github.com/libp2pchannel/signaling-server/turn"
)

// Handler holds shared state for all HTTP handlers.
type Handler struct {
	Manager *room.Manager
	Config  *config.Config
}

func NewHandler(cfg *config.Config) *Handler {
	return &Handler{
		Manager: room.NewManager(cfg.MaxSubscribers),
		Config:  cfg,
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

// HandleSignal dispatches POST /v1/signal requests.
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

	// Validate JWT from request body
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

	peer := h.Manager.EnsurePeer(req.PeerID)
	if err := h.Manager.CreateRoom(roomID, peer); err != nil {
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

	peer := h.Manager.EnsurePeer(req.PeerID)
	if err := h.Manager.JoinRoom(roomID, peer); err != nil {
		writeError(w, http.StatusConflict, err.Error())
		return
	}

	// Generate TURN credentials for subscriber
	username, password := turn.GenerateCredentials(
		h.Config.TURNSharedSecret, req.PeerID, h.Config.TURNCredTTL)

	// Also generate TURN credentials for the publisher
	rm := h.Manager.GetRoom(roomID)
	if rm != nil && rm.Publisher != nil {
		pubUser, pubPass := turn.GenerateCredentials(
			h.Config.TURNSharedSecret, rm.Publisher.ID, h.Config.TURNCredTTL)
		rm.Publisher.PushJSON("turn_credentials", model.TurnInfo{
			Username: pubUser,
			Password: pubPass,
			Server:   h.Config.TURNServerHost,
			Port:     h.Config.TURNServerPort,
			TTL:      h.Config.TURNCredTTL,
		})
	}

	resp := model.SignalResponse{
		Type:   "joined",
		RoomID: roomID,
		Turn: &model.TurnInfo{
			Username: username,
			Password: password,
			Server:   h.Config.TURNServerHost,
			Port:     h.Config.TURNServerPort,
			TTL:      h.Config.TURNCredTTL,
		},
	}

	writeJSON(w, http.StatusOK, resp)
}

func (h *Handler) handleLeaveRoom(w http.ResponseWriter, req *model.SignalRequest) {
	h.Manager.LeaveRoom(req.PeerID)
	writeJSON(w, http.StatusOK, model.SignalResponse{Type: "ok"})
}

// handleICEForward forwards an ICE offer/answer/gathering_done to the target peer via SSE.
func (h *Handler) handleICEForward(w http.ResponseWriter, req *model.SignalRequest, eventType string) {
	if req.To == "" {
		writeError(w, http.StatusBadRequest, "to required")
		return
	}

	payload := map[string]interface{}{
		"from": req.PeerID,
	}
	if req.SDP != "" {
		payload["sdp"] = req.SDP
	}

	data, _ := json.Marshal(payload)
	evt := model.SSEEvent{Type: eventType, Data: string(data)}

	if err := h.Manager.ForwardToPeer(req.To, evt); err != nil {
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
	evt := model.SSEEvent{Type: "ice_candidate", Data: string(data)}

	if err := h.Manager.ForwardToPeer(req.To, evt); err != nil {
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

	offer := &room.CachedICE{
		From:       req.PeerID,
		SDP:        req.SDP,
		Candidates: req.Candidates,
	}

	// Cache it for late-joining subscribers
	if req.RoomID != "" {
		h.Manager.CacheOffer(req.RoomID, offer)
	}

	data, _ := json.Marshal(offer)
	evt := model.SSEEvent{Type: "full_offer", Data: string(data)}

	if err := h.Manager.ForwardToPeer(req.To, evt); err != nil {
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
	evt := model.SSEEvent{Type: "full_answer", Data: string(data)}

	if err := h.Manager.ForwardToPeer(req.To, evt); err != nil {
		log.Printf("[handler] forward full_answer from %s to %s failed: %v",
			req.PeerID, req.To, err)
		writeError(w, http.StatusNotFound, err.Error())
		return
	}

	writeJSON(w, http.StatusOK, model.SignalResponse{Type: "ok"})
}
