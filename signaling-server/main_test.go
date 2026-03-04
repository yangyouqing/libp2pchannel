package main

import (
	"bufio"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/libp2pchannel/signaling-server/auth"
	"github.com/libp2pchannel/signaling-server/config"
	"github.com/libp2pchannel/signaling-server/handler"
	"github.com/libp2pchannel/signaling-server/middleware"
	"github.com/libp2pchannel/signaling-server/model"
	"github.com/libp2pchannel/signaling-server/pubsub"
	"github.com/libp2pchannel/signaling-server/store"
)

func setupTestServer(t *testing.T, maxSubs int) (*httptest.Server, *config.Config) {
	t.Helper()
	cfg := config.DefaultConfig()
	cfg.MaxSubscribers = maxSubs
	cfg.JWTSecret = "test-secret"
	cfg.AdminSecret = "test-admin"
	cfg.NodeID = "test-node"
	cfg.TURNServers = []config.TURNServerConfig{
		{Host: cfg.TURNServerHost, Port: cfg.TURNServerPort, Secret: cfg.TURNSharedSecret},
	}

	st := store.NewMemoryStore()
	ps := pubsub.NewLocalPubSub()
	h := handler.NewHandler(cfg, st, ps)
	authMW := middleware.Auth(cfg.JWTSecret, cfg.AdminSecret)

	mux := http.NewServeMux()
	mux.HandleFunc("/v1/signal", h.HandleSignal)
	mux.HandleFunc("/v1/events", h.HandleSSE)
	mux.HandleFunc("/v1/admin/peers/", h.HandleAdminPeerByID)
	mux.HandleFunc("/v1/admin/peers", h.HandleAdminPeers)
	mux.HandleFunc("/v1/admin/rooms/", h.HandleAdminRoomByID)
	mux.HandleFunc("/v1/admin/rooms", h.HandleAdminRooms)

	srv := httptest.NewServer(authMW(mux))
	return srv, cfg
}

func genToken(t *testing.T, peerID, secret string) string {
	t.Helper()
	tok, err := auth.GenerateToken(peerID, secret, 1*time.Hour)
	if err != nil {
		t.Fatalf("generate token: %v", err)
	}
	return tok
}

func postSignal(t *testing.T, baseURL string, req model.SignalRequest) model.SignalResponse {
	t.Helper()
	body, _ := json.Marshal(req)
	resp, err := http.Post(baseURL+"/v1/signal", "application/json",
		strings.NewReader(string(body)))
	if err != nil {
		t.Fatalf("POST /v1/signal: %v", err)
	}
	defer resp.Body.Close()

	var sresp model.SignalResponse
	json.NewDecoder(resp.Body).Decode(&sresp)
	return sresp
}

func readSSEEvent(t *testing.T, reader *bufio.Reader, timeout time.Duration) (string, string) {
	t.Helper()
	eventType := ""
	eventData := ""

	done := make(chan struct{})
	go func() {
		defer close(done)
		for {
			line, err := reader.ReadString('\n')
			if err != nil {
				return
			}
			line = strings.TrimRight(line, "\r\n")
			if line == "" {
				if eventType != "" || eventData != "" {
					return
				}
				continue
			}
			if strings.HasPrefix(line, "event: ") {
				eventType = strings.TrimPrefix(line, "event: ")
			} else if strings.HasPrefix(line, "data: ") {
				eventData = strings.TrimPrefix(line, "data: ")
			}
		}
	}()

	select {
	case <-done:
	case <-time.After(timeout):
		t.Fatalf("SSE event read timeout after %v", timeout)
	}
	return eventType, eventData
}

func TestCreateAndJoinRoom(t *testing.T) {
	srv, cfg := setupTestServer(t, 5)
	defer srv.Close()

	pubToken := genToken(t, "publisher-1", cfg.JWTSecret)
	subToken := genToken(t, "subscriber-1", cfg.JWTSecret)

	pubSSEResp, err := http.Get(srv.URL + "/v1/events?peer_id=publisher-1&token=" + pubToken)
	if err != nil {
		t.Fatalf("SSE connect: %v", err)
	}
	defer pubSSEResp.Body.Close()
	pubReader := bufio.NewReader(pubSSEResp.Body)

	resp := postSignal(t, srv.URL, model.SignalRequest{
		Type: "create_room", PeerID: "publisher-1", Token: pubToken, RoomID: "test-room",
	})
	if resp.Type != "room_created" {
		t.Fatalf("expected room_created, got %s: %s", resp.Type, resp.Error)
	}
	if resp.RoomID != "test-room" {
		t.Fatalf("expected room_id test-room, got %s", resp.RoomID)
	}
	t.Logf("Room created: %s", resp.RoomID)

	subSSEResp, err := http.Get(srv.URL + "/v1/events?peer_id=subscriber-1&token=" + subToken)
	if err != nil {
		t.Fatalf("SSE connect: %v", err)
	}
	defer subSSEResp.Body.Close()

	joinResp := postSignal(t, srv.URL, model.SignalRequest{
		Type: "join_room", PeerID: "subscriber-1", Token: subToken, RoomID: "test-room",
	})
	if joinResp.Type != "joined" {
		t.Fatalf("expected joined, got %s: %s", joinResp.Type, joinResp.Error)
	}
	if joinResp.Turn == nil {
		t.Fatal("expected TURN credentials in join response")
	}
	t.Logf("Join response: TURN user=%s server=%s:%d",
		joinResp.Turn.Username, joinResp.Turn.Server, joinResp.Turn.Port)

	if len(joinResp.TurnServers) == 0 {
		t.Fatal("expected TurnServers array in join response")
	}
	t.Logf("TurnServers count: %d", len(joinResp.TurnServers))

	evtType, evtData := readSSEEvent(t, pubReader, 5*time.Second)

	if evtType == "turn_credentials" {
		t.Logf("Publisher got TURN creds first (expected)")
		evtType, evtData = readSSEEvent(t, pubReader, 5*time.Second)
	}
	if evtType != "peer_joined" {
		t.Fatalf("expected peer_joined SSE event, got %s", evtType)
	}
	t.Logf("Publisher SSE: %s = %s", evtType, evtData)
}

func TestICEForwarding(t *testing.T) {
	srv, cfg := setupTestServer(t, 5)
	defer srv.Close()

	pubToken := genToken(t, "pub1", cfg.JWTSecret)
	subToken := genToken(t, "sub1", cfg.JWTSecret)

	pubSSEResp, _ := http.Get(srv.URL + "/v1/events?peer_id=pub1&token=" + pubToken)
	defer pubSSEResp.Body.Close()
	pubReader := bufio.NewReader(pubSSEResp.Body)

	subSSEResp, _ := http.Get(srv.URL + "/v1/events?peer_id=sub1&token=" + subToken)
	defer subSSEResp.Body.Close()
	subReader := bufio.NewReader(subSSEResp.Body)

	postSignal(t, srv.URL, model.SignalRequest{
		Type: "create_room", PeerID: "pub1", Token: pubToken, RoomID: "r1",
	})
	postSignal(t, srv.URL, model.SignalRequest{
		Type: "join_room", PeerID: "sub1", Token: subToken, RoomID: "r1",
	})

	readSSEEvent(t, pubReader, 5*time.Second)
	readSSEEvent(t, pubReader, 5*time.Second)

	resp := postSignal(t, srv.URL, model.SignalRequest{
		Type: "ice_offer", PeerID: "pub1", Token: pubToken,
		To: "sub1", RoomID: "r1", SDP: "v=0\r\ntest-offer\r\n",
	})
	if resp.Type != "ok" {
		t.Fatalf("expected ok, got %s: %s", resp.Type, resp.Error)
	}

	evtType, evtData := readSSEEvent(t, subReader, 5*time.Second)
	if evtType != "ice_offer" {
		t.Fatalf("expected ice_offer, got %s", evtType)
	}
	t.Logf("Subscriber got: %s = %s", evtType, evtData)

	resp = postSignal(t, srv.URL, model.SignalRequest{
		Type: "ice_answer", PeerID: "sub1", Token: subToken,
		To: "pub1", RoomID: "r1", SDP: "v=0\r\ntest-answer\r\n",
	})
	if resp.Type != "ok" {
		t.Fatalf("expected ok, got %s", resp.Type)
	}

	evtType, _ = readSSEEvent(t, pubReader, 5*time.Second)
	if evtType != "ice_answer" {
		t.Fatalf("expected ice_answer, got %s", evtType)
	}
	t.Logf("ICE offer/answer forwarded OK")
}

func TestAdminEndpoints(t *testing.T) {
	srv, cfg := setupTestServer(t, 5)
	defer srv.Close()

	pubToken := genToken(t, "admin-pub", cfg.JWTSecret)

	sseResp, _ := http.Get(srv.URL + "/v1/events?peer_id=admin-pub&token=" + pubToken)
	defer sseResp.Body.Close()

	postSignal(t, srv.URL, model.SignalRequest{
		Type: "create_room", PeerID: "admin-pub", Token: pubToken, RoomID: "admin-room",
	})

	req, _ := http.NewRequest("GET", srv.URL+"/v1/admin/peers", nil)
	req.Header.Set("Authorization", "Bearer "+cfg.AdminSecret)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("admin peers: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}

	var peersResp map[string][]model.PeerInfo
	json.NewDecoder(resp.Body).Decode(&peersResp)
	if len(peersResp["peers"]) == 0 {
		t.Fatal("expected at least one peer")
	}
	t.Logf("Admin peers: %+v", peersResp["peers"])

	req, _ = http.NewRequest("GET", srv.URL+"/v1/admin/rooms", nil)
	req.Header.Set("Authorization", "Bearer "+cfg.AdminSecret)
	resp, _ = http.DefaultClient.Do(req)
	defer resp.Body.Close()

	var roomsResp map[string][]model.RoomInfo
	json.NewDecoder(resp.Body).Decode(&roomsResp)
	if len(roomsResp["rooms"]) == 0 {
		t.Fatal("expected at least one room")
	}
	t.Logf("Admin rooms: %+v", roomsResp["rooms"])

	req, _ = http.NewRequest("GET", srv.URL+"/v1/admin/peers/admin-pub", nil)
	req.Header.Set("Authorization", "Bearer "+cfg.AdminSecret)
	resp, _ = http.DefaultClient.Do(req)
	defer resp.Body.Close()

	var peerInfo model.PeerInfo
	json.NewDecoder(resp.Body).Decode(&peerInfo)
	if !peerInfo.Online {
		t.Fatal("expected peer to be online")
	}
	t.Logf("Peer info: %+v", peerInfo)
}

func TestAdminAuthRequired(t *testing.T) {
	srv, _ := setupTestServer(t, 5)
	defer srv.Close()

	resp, _ := http.Get(srv.URL + "/v1/admin/peers")
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("expected 401, got %d", resp.StatusCode)
	}

	req, _ := http.NewRequest("GET", srv.URL+"/v1/admin/peers", nil)
	req.Header.Set("Authorization", "Bearer wrong-secret")
	resp, _ = http.DefaultClient.Do(req)
	if resp.StatusCode != http.StatusForbidden {
		t.Fatalf("expected 403, got %d", resp.StatusCode)
	}
}

func TestMaxSubscribers(t *testing.T) {
	srv, cfg := setupTestServer(t, 2)
	defer srv.Close()

	pubToken := genToken(t, "pub", cfg.JWTSecret)

	sseResp, _ := http.Get(srv.URL + "/v1/events?peer_id=pub&token=" + pubToken)
	defer sseResp.Body.Close()

	postSignal(t, srv.URL, model.SignalRequest{
		Type: "create_room", PeerID: "pub", Token: pubToken, RoomID: "limited",
	})

	for i := 1; i <= 2; i++ {
		subID := "sub" + string(rune('0'+i))
		subToken := genToken(t, subID, cfg.JWTSecret)
		sseR, _ := http.Get(srv.URL + "/v1/events?peer_id=" + subID + "&token=" + subToken)
		defer sseR.Body.Close()

		resp := postSignal(t, srv.URL, model.SignalRequest{
			Type: "join_room", PeerID: subID, Token: subToken, RoomID: "limited",
		})
		if resp.Type == "error" {
			t.Fatalf("subscriber %d should have been accepted: %s", i, resp.Error)
		}
	}

	sub3Token := genToken(t, "sub3", cfg.JWTSecret)
	resp := postSignal(t, srv.URL, model.SignalRequest{
		Type: "join_room", PeerID: "sub3", Token: sub3Token, RoomID: "limited",
	})
	if resp.Type != "error" {
		t.Fatalf("expected error for 3rd subscriber, got %s", resp.Type)
	}
	t.Logf("Max subscribers enforced: %s", resp.Error)
}
