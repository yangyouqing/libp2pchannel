package model

// SignalRequest is the JSON body sent by clients via POST /v1/signal.
type SignalRequest struct {
	Type       string   `json:"type"`
	PeerID     string   `json:"peer_id"`
	Token      string   `json:"token,omitempty"`
	To         string   `json:"to,omitempty"`
	RoomID     string   `json:"room_id,omitempty"`
	SDP        string   `json:"sdp,omitempty"`
	Candidate  string   `json:"candidate,omitempty"`
	Candidates []string `json:"candidates,omitempty"`
}

// SignalResponse is the JSON response returned by POST /v1/signal.
type SignalResponse struct {
	Type        string     `json:"type"`
	RoomID      string     `json:"room_id,omitempty"`
	Error       string     `json:"error,omitempty"`
	Turn        *TurnInfo  `json:"turn,omitempty"`
	TurnServers []TurnInfo `json:"turn_servers,omitempty"`
}

// TurnInfo carries TURN server credentials.
type TurnInfo struct {
	Username string `json:"username"`
	Password string `json:"password"`
	Server   string `json:"server"`
	Port     uint16 `json:"port"`
	TTL      uint32 `json:"ttl"`
}

// SSEEvent is pushed to clients over the event stream.
type SSEEvent struct {
	Type string
	Data string
}

// PeerInfo is the admin API representation of a connected peer.
type PeerInfo struct {
	ID          string `json:"id"`
	RoomID      string `json:"room_id,omitempty"`
	Role        string `json:"role,omitempty"`
	Online      bool   `json:"online"`
	OnlineSince string `json:"online_since,omitempty"`
	NodeID      string `json:"node_id,omitempty"`
}

// RoomInfo is the admin API representation of a room.
type RoomInfo struct {
	ID          string   `json:"id"`
	Publisher   string   `json:"publisher"`
	Subscribers []string `json:"subscribers"`
}
