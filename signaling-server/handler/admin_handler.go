package handler

import (
	"net/http"
	"strings"
)

// HandleAdminPeers serves GET /v1/admin/peers -- list all online peers.
func (h *Handler) HandleAdminPeers(w http.ResponseWriter, r *http.Request) {
	peers := h.Manager.ListPeers()
	writeJSON(w, http.StatusOK, map[string]interface{}{"peers": peers})
}

// HandleAdminPeerByID serves GET /v1/admin/peers/{id} -- single peer status.
func (h *Handler) HandleAdminPeerByID(w http.ResponseWriter, r *http.Request) {
	peerID := extractPathParam(r.URL.Path, "/v1/admin/peers/")
	if peerID == "" {
		writeError(w, http.StatusBadRequest, "peer id required")
		return
	}

	info := h.Manager.GetPeerInfo(peerID)
	if info == nil {
		writeJSON(w, http.StatusOK, map[string]interface{}{
			"id":     peerID,
			"online": false,
		})
		return
	}
	writeJSON(w, http.StatusOK, info)
}

// HandleAdminRooms serves GET /v1/admin/rooms -- list all rooms.
func (h *Handler) HandleAdminRooms(w http.ResponseWriter, r *http.Request) {
	rooms := h.Manager.ListRooms()
	writeJSON(w, http.StatusOK, map[string]interface{}{"rooms": rooms})
}

// HandleAdminRoomByID serves GET /v1/admin/rooms/{id} -- single room info.
func (h *Handler) HandleAdminRoomByID(w http.ResponseWriter, r *http.Request) {
	roomID := extractPathParam(r.URL.Path, "/v1/admin/rooms/")
	if roomID == "" {
		writeError(w, http.StatusBadRequest, "room id required")
		return
	}

	info := h.Manager.GetRoomInfo(roomID)
	if info == nil {
		writeError(w, http.StatusNotFound, "room not found")
		return
	}
	writeJSON(w, http.StatusOK, info)
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
