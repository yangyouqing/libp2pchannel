package handler

import (
	"net/http"
	"strings"
	"time"

	"github.com/libp2pchannel/signaling-server/model"
	"github.com/libp2pchannel/signaling-server/store"
)

func (h *Handler) HandleAdminPeers(w http.ResponseWriter, r *http.Request) {
	peers, err := h.Store.ListPeers()
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	infos := make([]model.PeerInfo, 0, len(peers))
	for _, p := range peers {
		infos = append(infos, peerMetaToInfo(&p))
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"peers": infos})
}

func (h *Handler) HandleAdminPeerByID(w http.ResponseWriter, r *http.Request) {
	peerID := extractPathParam(r.URL.Path, "/v1/admin/peers/")
	if peerID == "" {
		writeError(w, http.StatusBadRequest, "peer id required")
		return
	}

	p, err := h.Store.GetPeer(peerID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	if p == nil {
		writeJSON(w, http.StatusOK, map[string]interface{}{
			"id":     peerID,
			"online": false,
		})
		return
	}
	writeJSON(w, http.StatusOK, peerMetaToInfo(p))
}

func (h *Handler) HandleAdminRooms(w http.ResponseWriter, r *http.Request) {
	rooms, err := h.Store.ListRooms()
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	infos := make([]model.RoomInfo, 0, len(rooms))
	for _, rm := range rooms {
		infos = append(infos, model.RoomInfo{
			ID:          rm.ID,
			Publisher:   rm.PublisherID,
			Subscribers: rm.SubscriberIDs,
		})
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"rooms": infos})
}

func (h *Handler) HandleAdminRoomByID(w http.ResponseWriter, r *http.Request) {
	roomID := extractPathParam(r.URL.Path, "/v1/admin/rooms/")
	if roomID == "" {
		writeError(w, http.StatusBadRequest, "room id required")
		return
	}

	rm, err := h.Store.GetRoom(roomID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	if rm == nil {
		http.Error(w, `{"error":"room not found"}`, http.StatusNotFound)
		return
	}
	writeJSON(w, http.StatusOK, model.RoomInfo{
		ID:          rm.ID,
		Publisher:   rm.PublisherID,
		Subscribers: rm.SubscriberIDs,
	})
}

func extractPathParam(path, prefix string) string {
	if !strings.HasPrefix(path, prefix) {
		return ""
	}
	param := strings.TrimPrefix(path, prefix)
	if i := strings.Index(param, "/"); i >= 0 {
		param = param[:i]
	}
	return param
}

func peerMetaToInfo(p *store.PeerMeta) model.PeerInfo {
	role := "standalone"
	if p.RoomID != "" {
		if p.IsPublisher {
			role = "publisher"
		} else {
			role = "subscriber"
		}
	}
	return model.PeerInfo{
		ID:          p.ID,
		RoomID:      p.RoomID,
		Role:        role,
		Online:      p.NodeID != "",
		OnlineSince: p.OnlineAt.Format(time.RFC3339),
		NodeID:      p.NodeID,
	}
}
