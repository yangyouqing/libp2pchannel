package room

import (
	"encoding/json"
	"fmt"
	"log"
	"sync"
	"time"

	"github.com/libp2pchannel/signaling-server/model"
)

// Peer represents a connected client with an SSE event channel.
type Peer struct {
	ID          string
	RoomID      string
	IsPublisher bool
	OnlineAt    time.Time
	Events      chan model.SSEEvent
	mu          sync.Mutex
}

// PushEvent sends an SSE event to the peer's event channel (non-blocking).
func (p *Peer) PushEvent(evt model.SSEEvent) {
	select {
	case p.Events <- evt:
	default:
		log.Printf("[room] event channel full for peer %s, dropping event %s", p.ID, evt.Type)
	}
}

// PushJSON marshals data to JSON and pushes an SSE event.
func (p *Peer) PushJSON(eventType string, data interface{}) {
	b, err := json.Marshal(data)
	if err != nil {
		log.Printf("[room] marshal error for peer %s: %v", p.ID, err)
		return
	}
	p.PushEvent(model.SSEEvent{Type: eventType, Data: string(b)})
}

// Room holds a publisher and its subscribers.
type Room struct {
	ID             string
	Publisher      *Peer
	Subscribers    map[string]*Peer
	MaxSubscribers int
	CachedOffer    *CachedICE // Publisher's pre-gathered offer
	mu             sync.RWMutex
}

// CachedICE stores a batched SDP + candidates for forwarding.
type CachedICE struct {
	From       string   `json:"from"`
	SDP        string   `json:"sdp"`
	Candidates []string `json:"candidates"`
}

func NewRoom(id string, publisher *Peer, maxSubs int) *Room {
	return &Room{
		ID:             id,
		Publisher:       publisher,
		Subscribers:    make(map[string]*Peer),
		MaxSubscribers: maxSubs,
	}
}

func (r *Room) AddSubscriber(peer *Peer) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if len(r.Subscribers) >= r.MaxSubscribers {
		return fmt.Errorf("room %s is full (%d/%d)", r.ID, len(r.Subscribers), r.MaxSubscribers)
	}
	r.Subscribers[peer.ID] = peer
	return nil
}

func (r *Room) RemoveSubscriber(peerID string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.Subscribers, peerID)
}

func (r *Room) GetSubscriber(peerID string) *Peer {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.Subscribers[peerID]
}

func (r *Room) SubscriberCount() int {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return len(r.Subscribers)
}

func (r *Room) SubscriberIDs() []string {
	r.mu.RLock()
	defer r.mu.RUnlock()
	ids := make([]string, 0, len(r.Subscribers))
	for id := range r.Subscribers {
		ids = append(ids, id)
	}
	return ids
}

// Manager manages all rooms and peers (in-memory, single-server).
type Manager struct {
	rooms          map[string]*Room
	peers          map[string]*Peer
	maxSubscribers int
	mu             sync.RWMutex
}

func NewManager(maxSubscribers int) *Manager {
	return &Manager{
		rooms:          make(map[string]*Room),
		peers:          make(map[string]*Peer),
		maxSubscribers: maxSubscribers,
	}
}

// RegisterSSE registers a peer with an SSE channel. Called when SSE connection opens.
func (m *Manager) RegisterSSE(peerID string, events chan model.SSEEvent) *Peer {
	m.mu.Lock()
	defer m.mu.Unlock()

	peer, exists := m.peers[peerID]
	if exists {
		// Peer already exists (re-connect); replace the event channel
		peer.Events = events
		peer.OnlineAt = time.Now()
		return peer
	}

	peer = &Peer{
		ID:       peerID,
		OnlineAt: time.Now(),
		Events:   events,
	}
	m.peers[peerID] = peer
	log.Printf("[room] peer %s registered (SSE)", peerID)
	return peer
}

// UnregisterSSE removes a peer's SSE presence. Called when SSE connection closes.
func (m *Manager) UnregisterSSE(peerID string) {
	m.mu.Lock()
	defer m.mu.Unlock()

	peer, ok := m.peers[peerID]
	if !ok {
		return
	}

	if peer.RoomID != "" {
		if room, exists := m.rooms[peer.RoomID]; exists {
			if peer.IsPublisher {
				room.mu.RLock()
				for _, sub := range room.Subscribers {
					sub.PushJSON("peer_left", map[string]string{"peer_id": peerID})
				}
				room.mu.RUnlock()
				delete(m.rooms, peer.RoomID)
				log.Printf("[room] room %s removed (publisher %s disconnected)", peer.RoomID, peerID)
			} else {
				room.RemoveSubscriber(peerID)
				if room.Publisher != nil {
					room.Publisher.PushJSON("peer_left", map[string]string{"peer_id": peerID})
				}
			}
		}
	}

	delete(m.peers, peerID)
	log.Printf("[room] peer %s unregistered", peerID)
}

// EnsurePeer returns the existing peer or creates a placeholder without SSE.
func (m *Manager) EnsurePeer(peerID string) *Peer {
	m.mu.Lock()
	defer m.mu.Unlock()
	peer, ok := m.peers[peerID]
	if ok {
		return peer
	}
	peer = &Peer{
		ID:       peerID,
		OnlineAt: time.Now(),
	}
	m.peers[peerID] = peer
	return peer
}

func (m *Manager) GetPeer(peerID string) *Peer {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.peers[peerID]
}

func (m *Manager) CreateRoom(roomID string, publisher *Peer) error {
	m.mu.Lock()
	defer m.mu.Unlock()

	if _, exists := m.rooms[roomID]; exists {
		return fmt.Errorf("room %s already exists", roomID)
	}

	room := NewRoom(roomID, publisher, m.maxSubscribers)
	m.rooms[roomID] = room
	publisher.RoomID = roomID
	publisher.IsPublisher = true

	log.Printf("[room] created room %s by publisher %s", roomID, publisher.ID)
	return nil
}

func (m *Manager) JoinRoom(roomID string, subscriber *Peer) error {
	m.mu.RLock()
	room, exists := m.rooms[roomID]
	m.mu.RUnlock()

	if !exists {
		return fmt.Errorf("room %s not found", roomID)
	}

	if err := room.AddSubscriber(subscriber); err != nil {
		return err
	}

	subscriber.RoomID = roomID
	subscriber.IsPublisher = false

	log.Printf("[room] subscriber %s joined room %s (%d/%d)",
		subscriber.ID, roomID, room.SubscriberCount(), room.MaxSubscribers)

	// Notify publisher
	if room.Publisher != nil {
		room.Publisher.PushJSON("peer_joined", map[string]string{"peer_id": subscriber.ID})
	}

	return nil
}

func (m *Manager) LeaveRoom(peerID string) {
	m.mu.Lock()
	defer m.mu.Unlock()

	peer, ok := m.peers[peerID]
	if !ok || peer.RoomID == "" {
		return
	}

	room, exists := m.rooms[peer.RoomID]
	if !exists {
		peer.RoomID = ""
		return
	}

	if peer.IsPublisher {
		room.mu.RLock()
		for _, sub := range room.Subscribers {
			sub.PushJSON("peer_left", map[string]string{"peer_id": peerID})
		}
		room.mu.RUnlock()
		delete(m.rooms, peer.RoomID)
	} else {
		room.RemoveSubscriber(peerID)
		if room.Publisher != nil {
			room.Publisher.PushJSON("peer_left", map[string]string{"peer_id": peerID})
		}
	}

	peer.RoomID = ""
	peer.IsPublisher = false
}

func (m *Manager) GetRoom(roomID string) *Room {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.rooms[roomID]
}

// CacheOffer stores the publisher's full ICE offer for the given room.
func (m *Manager) CacheOffer(roomID string, offer *CachedICE) {
	m.mu.RLock()
	room, exists := m.rooms[roomID]
	m.mu.RUnlock()
	if !exists {
		return
	}
	room.mu.Lock()
	room.CachedOffer = offer
	room.mu.Unlock()
}

// GetCachedOffer returns the cached publisher offer, if any.
func (m *Manager) GetCachedOffer(roomID string) *CachedICE {
	m.mu.RLock()
	room, exists := m.rooms[roomID]
	m.mu.RUnlock()
	if !exists {
		return nil
	}
	room.mu.RLock()
	defer room.mu.RUnlock()
	return room.CachedOffer
}

// ForwardToPeer sends an SSE event to a specific peer.
func (m *Manager) ForwardToPeer(peerID string, evt model.SSEEvent) error {
	m.mu.RLock()
	peer, ok := m.peers[peerID]
	m.mu.RUnlock()
	if !ok {
		return fmt.Errorf("peer %s not found", peerID)
	}
	peer.PushEvent(evt)
	return nil
}

// ListPeers returns info about all online peers.
func (m *Manager) ListPeers() []model.PeerInfo {
	m.mu.RLock()
	defer m.mu.RUnlock()

	result := make([]model.PeerInfo, 0, len(m.peers))
	for _, p := range m.peers {
		role := "standalone"
		if p.RoomID != "" {
			if p.IsPublisher {
				role = "publisher"
			} else {
				role = "subscriber"
			}
		}
		result = append(result, model.PeerInfo{
			ID:          p.ID,
			RoomID:      p.RoomID,
			Role:        role,
			Online:      p.Events != nil,
			OnlineSince: p.OnlineAt.Format(time.RFC3339),
		})
	}
	return result
}

// ListRooms returns info about all active rooms.
func (m *Manager) ListRooms() []model.RoomInfo {
	m.mu.RLock()
	defer m.mu.RUnlock()

	result := make([]model.RoomInfo, 0, len(m.rooms))
	for _, r := range m.rooms {
		pubID := ""
		if r.Publisher != nil {
			pubID = r.Publisher.ID
		}
		result = append(result, model.RoomInfo{
			ID:          r.ID,
			Publisher:   pubID,
			Subscribers: r.SubscriberIDs(),
		})
	}
	return result
}

// GetPeerInfo returns admin info for a single peer.
func (m *Manager) GetPeerInfo(peerID string) *model.PeerInfo {
	m.mu.RLock()
	defer m.mu.RUnlock()

	p, ok := m.peers[peerID]
	if !ok {
		return nil
	}
	role := "standalone"
	if p.RoomID != "" {
		if p.IsPublisher {
			role = "publisher"
		} else {
			role = "subscriber"
		}
	}
	return &model.PeerInfo{
		ID:          p.ID,
		RoomID:      p.RoomID,
		Role:        role,
		Online:      p.Events != nil,
		OnlineSince: p.OnlineAt.Format(time.RFC3339),
	}
}

// GetRoomInfo returns admin info for a single room.
func (m *Manager) GetRoomInfo(roomID string) *model.RoomInfo {
	m.mu.RLock()
	defer m.mu.RUnlock()

	r, ok := m.rooms[roomID]
	if !ok {
		return nil
	}
	pubID := ""
	if r.Publisher != nil {
		pubID = r.Publisher.ID
	}
	return &model.RoomInfo{
		ID:          r.ID,
		Publisher:   pubID,
		Subscribers: r.SubscriberIDs(),
	}
}
