package store

import (
	"fmt"
	"log"
	"sync"
	"time"
)

type memPeer struct {
	meta PeerMeta
}

type memRoom struct {
	id             string
	publisherID    string
	subscribers    map[string]bool
	maxSubscribers int
	cachedOffer    *CachedICE
}

type MemoryStore struct {
	peers map[string]*memPeer
	rooms map[string]*memRoom
	mu    sync.RWMutex
}

func NewMemoryStore() *MemoryStore {
	return &MemoryStore{
		peers: make(map[string]*memPeer),
		rooms: make(map[string]*memRoom),
	}
}

func (m *MemoryStore) RegisterPeer(peerID, nodeID string) error {
	m.mu.Lock()
	defer m.mu.Unlock()

	if p, ok := m.peers[peerID]; ok {
		p.meta.NodeID = nodeID
		p.meta.OnlineAt = time.Now()
		return nil
	}

	m.peers[peerID] = &memPeer{
		meta: PeerMeta{
			ID:       peerID,
			NodeID:   nodeID,
			OnlineAt: time.Now(),
		},
	}
	log.Printf("[store] peer %s registered on node %s", peerID, nodeID)
	return nil
}

func (m *MemoryStore) UnregisterPeer(peerID string) (*PeerMeta, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	p, ok := m.peers[peerID]
	if !ok {
		return nil, nil
	}
	meta := p.meta

	if meta.RoomID != "" {
		if room, exists := m.rooms[meta.RoomID]; exists {
			if meta.IsPublisher {
				delete(m.rooms, meta.RoomID)
				log.Printf("[store] room %s removed (publisher %s disconnected)", meta.RoomID, peerID)
			} else {
				delete(room.subscribers, peerID)
			}
		}
	}

	delete(m.peers, peerID)
	log.Printf("[store] peer %s unregistered", peerID)
	return &meta, nil
}

func (m *MemoryStore) GetPeer(peerID string) (*PeerMeta, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()

	p, ok := m.peers[peerID]
	if !ok {
		return nil, nil
	}
	meta := p.meta
	return &meta, nil
}

func (m *MemoryStore) EnsurePeer(peerID, nodeID string) (*PeerMeta, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	if p, ok := m.peers[peerID]; ok {
		meta := p.meta
		return &meta, nil
	}

	p := &memPeer{
		meta: PeerMeta{
			ID:       peerID,
			NodeID:   nodeID,
			OnlineAt: time.Now(),
		},
	}
	m.peers[peerID] = p
	meta := p.meta
	return &meta, nil
}

func (m *MemoryStore) CreateRoom(roomID, publisherID string, maxSubs int) error {
	m.mu.Lock()
	defer m.mu.Unlock()

	if _, exists := m.rooms[roomID]; exists {
		return fmt.Errorf("room %s already exists", roomID)
	}

	m.rooms[roomID] = &memRoom{
		id:             roomID,
		publisherID:    publisherID,
		subscribers:    make(map[string]bool),
		maxSubscribers: maxSubs,
	}

	if p, ok := m.peers[publisherID]; ok {
		p.meta.RoomID = roomID
		p.meta.IsPublisher = true
	}

	log.Printf("[store] created room %s by publisher %s", roomID, publisherID)
	return nil
}

func (m *MemoryStore) JoinRoom(roomID, subscriberID string) (*RoomMeta, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	room, exists := m.rooms[roomID]
	if !exists {
		return nil, fmt.Errorf("room %s not found", roomID)
	}

	if len(room.subscribers) >= room.maxSubscribers {
		return nil, fmt.Errorf("room %s is full (%d/%d)", roomID, len(room.subscribers), room.maxSubscribers)
	}

	room.subscribers[subscriberID] = true

	if p, ok := m.peers[subscriberID]; ok {
		p.meta.RoomID = roomID
		p.meta.IsPublisher = false
	}

	log.Printf("[store] subscriber %s joined room %s (%d/%d)",
		subscriberID, roomID, len(room.subscribers), room.maxSubscribers)

	return m.roomMetaLocked(room), nil
}

func (m *MemoryStore) LeaveRoom(peerID string) error {
	m.mu.Lock()
	defer m.mu.Unlock()

	p, ok := m.peers[peerID]
	if !ok || p.meta.RoomID == "" {
		return nil
	}

	room, exists := m.rooms[p.meta.RoomID]
	if !exists {
		p.meta.RoomID = ""
		return nil
	}

	if p.meta.IsPublisher {
		delete(m.rooms, p.meta.RoomID)
	} else {
		delete(room.subscribers, peerID)
	}

	p.meta.RoomID = ""
	p.meta.IsPublisher = false
	return nil
}

func (m *MemoryStore) GetRoom(roomID string) (*RoomMeta, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()

	room, exists := m.rooms[roomID]
	if !exists {
		return nil, nil
	}
	return m.roomMetaLocked(room), nil
}

func (m *MemoryStore) CacheOffer(roomID string, offer *CachedICE) error {
	m.mu.Lock()
	defer m.mu.Unlock()

	room, exists := m.rooms[roomID]
	if !exists {
		return fmt.Errorf("room %s not found", roomID)
	}
	room.cachedOffer = offer
	return nil
}

func (m *MemoryStore) GetCachedOffer(roomID string) (*CachedICE, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()

	room, exists := m.rooms[roomID]
	if !exists {
		return nil, nil
	}
	return room.cachedOffer, nil
}

func (m *MemoryStore) ListPeers() ([]PeerMeta, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()

	result := make([]PeerMeta, 0, len(m.peers))
	for _, p := range m.peers {
		result = append(result, p.meta)
	}
	return result, nil
}

func (m *MemoryStore) ListRooms() ([]RoomMeta, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()

	result := make([]RoomMeta, 0, len(m.rooms))
	for _, r := range m.rooms {
		result = append(result, *m.roomMetaLocked(r))
	}
	return result, nil
}

func (m *MemoryStore) roomMetaLocked(r *memRoom) *RoomMeta {
	subs := make([]string, 0, len(r.subscribers))
	for id := range r.subscribers {
		subs = append(subs, id)
	}
	return &RoomMeta{
		ID:             r.id,
		PublisherID:    r.publisherID,
		SubscriberIDs:  subs,
		MaxSubscribers: r.maxSubscribers,
		CachedOffer:    r.cachedOffer,
	}
}
