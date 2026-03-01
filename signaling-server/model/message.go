package model

const (
	MsgCreateRoom      = 1
	MsgJoinRoom        = 2
	MsgLeaveRoom       = 3
	MsgICEOffer        = 4
	MsgICEAnswer       = 5
	MsgICECandidate    = 6
	MsgGatheringDone   = 7
	MsgTurnCredentials = 8
	MsgRoomInfo        = 9
	MsgHeartbeat       = 10
	MsgError           = 11
	MsgPeerJoined      = 12
	MsgPeerLeft        = 13
)

type SignalMessage struct {
	Type         int    `json:"type"`
	From         string `json:"from"`
	To           string `json:"to"`
	Room         string `json:"room"`
	SDP          string `json:"sdp,omitempty"`
	Candidate    string `json:"candidate,omitempty"`
	TurnUsername string `json:"turn_username,omitempty"`
	TurnPassword string `json:"turn_password,omitempty"`
	TurnServer   string `json:"turn_server,omitempty"`
	TurnPort     uint16 `json:"turn_port,omitempty"`
	TurnTTL      uint32 `json:"turn_ttl,omitempty"`
	Error        string `json:"error,omitempty"`
}
