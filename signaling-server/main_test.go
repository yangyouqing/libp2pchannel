package main

import (
	"encoding/binary"
	"encoding/json"
	"net"
	"testing"
	"time"

	"github.com/libp2pchannel/signaling-server/config"
	"github.com/libp2pchannel/signaling-server/handler"
	"github.com/libp2pchannel/signaling-server/model"
)

func sendMsg(t *testing.T, conn net.Conn, msg *model.SignalMessage) {
	t.Helper()
	data, err := json.Marshal(msg)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	lenBuf := make([]byte, 4)
	binary.BigEndian.PutUint32(lenBuf, uint32(len(data)))
	conn.Write(lenBuf)
	conn.Write(data)
}

func recvMsg(t *testing.T, conn net.Conn) *model.SignalMessage {
	t.Helper()
	conn.SetReadDeadline(time.Now().Add(5 * time.Second))

	lenBuf := make([]byte, 4)
	if _, err := conn.Read(lenBuf); err != nil {
		t.Fatalf("read len: %v", err)
	}
	msgLen := binary.BigEndian.Uint32(lenBuf)
	data := make([]byte, msgLen)
	n := 0
	for n < int(msgLen) {
		nn, err := conn.Read(data[n:])
		if err != nil {
			t.Fatalf("read data: %v", err)
		}
		n += nn
	}

	var msg model.SignalMessage
	if err := json.Unmarshal(data[:n], &msg); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	return &msg
}

func TestSignalingEndToEnd(t *testing.T) {
	cfg := config.DefaultConfig()
	cfg.ListenAddr = "127.0.0.1:0"

	h := handler.NewSignalingHandler(cfg)

	listener, err := net.Listen("tcp", cfg.ListenAddr)
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer listener.Close()

	addr := listener.Addr().String()
	t.Logf("Server listening on %s", addr)

	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go h.HandleConnection(conn)
		}
	}()

	// --- Publisher connects and creates room ---
	pubConn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatalf("pub connect: %v", err)
	}
	defer pubConn.Close()

	sendMsg(t, pubConn, &model.SignalMessage{
		Type: model.MsgCreateRoom,
		From: "publisher-1",
		Room: "test-room",
	})

	roomInfo := recvMsg(t, pubConn)
	if roomInfo.Type != model.MsgRoomInfo {
		t.Fatalf("expected RoomInfo, got type %d", roomInfo.Type)
	}
	if roomInfo.Room != "test-room" {
		t.Fatalf("expected room 'test-room', got '%s'", roomInfo.Room)
	}
	t.Logf("Room created: %s", roomInfo.Room)

	// --- Subscriber connects and joins room ---
	subConn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatalf("sub connect: %v", err)
	}
	defer subConn.Close()

	sendMsg(t, subConn, &model.SignalMessage{
		Type: model.MsgJoinRoom,
		From: "subscriber-1",
		Room: "test-room",
	})

	// Subscriber should receive TURN credentials
	turnCreds := recvMsg(t, subConn)
	if turnCreds.Type != model.MsgTurnCredentials {
		t.Fatalf("expected TurnCredentials, got type %d", turnCreds.Type)
	}
	if turnCreds.TurnServer == "" {
		t.Fatal("TURN server not set")
	}
	if turnCreds.TurnUsername == "" || turnCreds.TurnPassword == "" {
		t.Fatal("TURN credentials empty")
	}
	t.Logf("TURN credentials: user=%s server=%s:%d ttl=%d",
		turnCreds.TurnUsername, turnCreds.TurnServer, turnCreds.TurnPort, turnCreds.TurnTTL)

	// Publisher should receive PeerJoined + TURN credentials
	pubMsg1 := recvMsg(t, pubConn)
	pubMsg2 := recvMsg(t, pubConn)

	var peerJoined, pubTurn *model.SignalMessage
	for _, m := range []*model.SignalMessage{pubMsg1, pubMsg2} {
		switch m.Type {
		case model.MsgPeerJoined:
			peerJoined = m
		case model.MsgTurnCredentials:
			pubTurn = m
		}
	}

	if peerJoined == nil || peerJoined.From != "subscriber-1" {
		t.Fatal("publisher did not receive PeerJoined")
	}
	t.Logf("Publisher notified of subscriber: %s", peerJoined.From)

	if pubTurn == nil {
		t.Fatal("publisher did not receive TURN credentials")
	}

	// --- ICE Offer/Answer exchange ---
	sendMsg(t, pubConn, &model.SignalMessage{
		Type: model.MsgICEOffer,
		From: "publisher-1",
		To:   "subscriber-1",
		Room: "test-room",
		SDP:  "v=0\r\na=ice-ufrag:AAAA\r\na=ice-pwd:BBBBBBBB\r\n",
	})

	offer := recvMsg(t, subConn)
	if offer.Type != model.MsgICEOffer {
		t.Fatalf("expected ICEOffer, got type %d", offer.Type)
	}
	if offer.From != "publisher-1" {
		t.Fatalf("expected from 'publisher-1', got '%s'", offer.From)
	}
	t.Logf("Subscriber received ICE offer from %s", offer.From)

	sendMsg(t, subConn, &model.SignalMessage{
		Type: model.MsgICEAnswer,
		From: "subscriber-1",
		To:   "publisher-1",
		Room: "test-room",
		SDP:  "v=0\r\na=ice-ufrag:CCCC\r\na=ice-pwd:DDDDDDDD\r\n",
	})

	answer := recvMsg(t, pubConn)
	if answer.Type != model.MsgICEAnswer {
		t.Fatalf("expected ICEAnswer, got type %d", answer.Type)
	}
	t.Logf("Publisher received ICE answer from %s", answer.From)

	// --- ICE Candidate exchange ---
	sendMsg(t, pubConn, &model.SignalMessage{
		Type:      model.MsgICECandidate,
		From:      "publisher-1",
		To:        "subscriber-1",
		Candidate: "candidate:1 1 UDP 2122252543 192.168.1.100 50000 typ host",
	})

	cand := recvMsg(t, subConn)
	if cand.Type != model.MsgICECandidate {
		t.Fatalf("expected ICECandidate, got type %d", cand.Type)
	}
	t.Logf("Candidate forwarded: %s", cand.Candidate)

	// --- Gathering done ---
	sendMsg(t, pubConn, &model.SignalMessage{
		Type: model.MsgGatheringDone,
		From: "publisher-1",
		To:   "subscriber-1",
	})

	gd := recvMsg(t, subConn)
	if gd.Type != model.MsgGatheringDone {
		t.Fatalf("expected GatheringDone, got type %d", gd.Type)
	}
	t.Logf("Gathering done forwarded")

	// --- Heartbeat ---
	sendMsg(t, pubConn, &model.SignalMessage{
		Type: model.MsgHeartbeat,
		From: "publisher-1",
	})

	hb := recvMsg(t, pubConn)
	if hb.Type != model.MsgHeartbeat {
		t.Fatalf("expected Heartbeat reply, got type %d", hb.Type)
	}
	t.Logf("Heartbeat OK")

	t.Log("=== All signaling tests passed ===")
}

func TestMaxSubscribers(t *testing.T) {
	cfg := config.DefaultConfig()
	cfg.ListenAddr = "127.0.0.1:0"
	cfg.MaxSubscribers = 2

	h := handler.NewSignalingHandler(cfg)

	listener, err := net.Listen("tcp", cfg.ListenAddr)
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer listener.Close()

	addr := listener.Addr().String()

	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go h.HandleConnection(conn)
		}
	}()

	// Publisher creates room
	pubConn, _ := net.Dial("tcp", addr)
	defer pubConn.Close()
	sendMsg(t, pubConn, &model.SignalMessage{
		Type: model.MsgCreateRoom, From: "pub", Room: "limited-room",
	})
	recvMsg(t, pubConn)

	// Two subscribers should succeed
	for i := 1; i <= 2; i++ {
		conn, _ := net.Dial("tcp", addr)
		defer conn.Close()
		sendMsg(t, conn, &model.SignalMessage{
			Type: model.MsgJoinRoom, From: "sub" + string(rune('0'+i)), Room: "limited-room",
		})
		msg := recvMsg(t, conn)
		if msg.Type == model.MsgError {
			t.Fatalf("subscriber %d should have been accepted", i)
		}
		// Drain publisher PeerJoined + TURN
		recvMsg(t, pubConn)
		recvMsg(t, pubConn)
	}

	// Third subscriber should be rejected
	conn3, _ := net.Dial("tcp", addr)
	defer conn3.Close()
	sendMsg(t, conn3, &model.SignalMessage{
		Type: model.MsgJoinRoom, From: "sub3", Room: "limited-room",
	})
	msg3 := recvMsg(t, conn3)
	if msg3.Type != model.MsgError {
		t.Fatalf("expected error for 3rd subscriber, got type %d", msg3.Type)
	}
	t.Logf("Max subscribers enforced: %s", msg3.SDP)
}
