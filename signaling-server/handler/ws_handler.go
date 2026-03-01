package handler

import (
	"fmt"
	"log"
	"net"
	"time"

	"github.com/libp2pchannel/signaling-server/config"
	"github.com/libp2pchannel/signaling-server/model"
	"github.com/libp2pchannel/signaling-server/room"
	"github.com/libp2pchannel/signaling-server/turn"
)

type SignalingHandler struct {
	manager *room.Manager
	config  *config.Config
}

func NewSignalingHandler(cfg *config.Config) *SignalingHandler {
	return &SignalingHandler{
		manager: room.NewManager(cfg.MaxSubscribers),
		config:  cfg,
	}
}

func (h *SignalingHandler) HandleConnection(conn net.Conn) {
	defer conn.Close()

	// First message must identify the peer
	firstMsg, err := room.ReadMessage(conn)
	if err != nil {
		log.Printf("[handler] failed to read first message: %v", err)
		return
	}
	if firstMsg.From == "" {
		log.Printf("[handler] first message missing 'from' field")
		return
	}

	peer := &room.Peer{
		ID:       firstMsg.From,
		Conn:     conn,
		LastPing: time.Now(),
	}
	h.manager.RegisterPeer(peer)
	defer h.manager.UnregisterPeer(peer.ID)

	log.Printf("[handler] peer %s connected", peer.ID)

	// Process the first message
	h.processMessage(peer, firstMsg)

	// Read loop
	for {
		conn.SetReadDeadline(time.Now().Add(time.Duration(h.config.HeartbeatTimeout*2) * time.Second))
		msg, err := room.ReadMessage(conn)
		if err != nil {
			log.Printf("[handler] peer %s read error: %v", peer.ID, err)
			return
		}
		peer.LastPing = time.Now()
		h.processMessage(peer, msg)
	}
}

func (h *SignalingHandler) processMessage(peer *room.Peer, msg *model.SignalMessage) {
	switch msg.Type {
	case model.MsgCreateRoom:
		h.handleCreateRoom(peer, msg)
	case model.MsgJoinRoom:
		h.handleJoinRoom(peer, msg)
	case model.MsgLeaveRoom:
		h.handleLeaveRoom(peer, msg)
	case model.MsgICEOffer, model.MsgICEAnswer, model.MsgICECandidate, model.MsgGatheringDone:
		h.handleICEMessage(peer, msg)
	case model.MsgHeartbeat:
		peer.Send(&model.SignalMessage{
			Type: model.MsgHeartbeat,
			From: "server",
			To:   peer.ID,
		})
	default:
		log.Printf("[handler] unknown message type %d from %s", msg.Type, peer.ID)
	}
}

func (h *SignalingHandler) handleCreateRoom(peer *room.Peer, msg *model.SignalMessage) {
	roomID := msg.Room
	if roomID == "" {
		roomID = fmt.Sprintf("room-%d", time.Now().UnixNano())
	}

	if err := h.manager.CreateRoom(roomID, peer); err != nil {
		peer.Send(&model.SignalMessage{
			Type:  model.MsgError,
			From:  "server",
			To:    peer.ID,
			SDP:   err.Error(),
		})
		return
	}

	peer.Send(&model.SignalMessage{
		Type: model.MsgRoomInfo,
		From: "server",
		To:   peer.ID,
		Room: roomID,
	})
}

func (h *SignalingHandler) handleJoinRoom(peer *room.Peer, msg *model.SignalMessage) {
	roomID := msg.Room
	if roomID == "" {
		peer.Send(&model.SignalMessage{
			Type: model.MsgError,
			From: "server",
			SDP:  "room ID required",
		})
		return
	}

	if err := h.manager.JoinRoom(roomID, peer); err != nil {
		peer.Send(&model.SignalMessage{
			Type: model.MsgError,
			From: "server",
			SDP:  err.Error(),
		})
		return
	}

	// Send TURN credentials to the subscriber
	username, password := turn.GenerateCredentials(
		h.config.TURNSharedSecret, peer.ID, h.config.TURNCredTTL)

	peer.Send(&model.SignalMessage{
		Type:         model.MsgTurnCredentials,
		From:         "server",
		To:           peer.ID,
		TurnUsername: username,
		TurnPassword: password,
		TurnServer:   h.config.TURNServerHost,
		TurnPort:     h.config.TURNServerPort,
		TurnTTL:      h.config.TURNCredTTL,
	})

	// Also send TURN credentials to the publisher
	r := h.manager.GetRoom(roomID)
	if r != nil && r.Publisher != nil {
		pubUser, pubPass := turn.GenerateCredentials(
			h.config.TURNSharedSecret, r.Publisher.ID, h.config.TURNCredTTL)
		r.Publisher.Send(&model.SignalMessage{
			Type:         model.MsgTurnCredentials,
			From:         "server",
			To:           r.Publisher.ID,
			TurnUsername: pubUser,
			TurnPassword: pubPass,
			TurnServer:   h.config.TURNServerHost,
			TurnPort:     h.config.TURNServerPort,
			TurnTTL:      h.config.TURNCredTTL,
		})
	}
}

func (h *SignalingHandler) handleLeaveRoom(peer *room.Peer, msg *model.SignalMessage) {
	// UnregisterPeer handles room cleanup
	h.manager.UnregisterPeer(peer.ID)
	// Re-register as a standalone peer (still connected)
	h.manager.RegisterPeer(peer)
	peer.RoomID = ""
}

func (h *SignalingHandler) handleICEMessage(peer *room.Peer, msg *model.SignalMessage) {
	if msg.To == "" {
		log.Printf("[handler] ICE message from %s missing 'to' field", peer.ID)
		return
	}

	msg.From = peer.ID

	if err := h.manager.ForwardMessage(msg); err != nil {
		log.Printf("[handler] failed to forward ICE message from %s to %s: %v",
			peer.ID, msg.To, err)
		peer.Send(&model.SignalMessage{
			Type: model.MsgError,
			From: "server",
			SDP:  fmt.Sprintf("forward failed: %v", err),
		})
	}
}
